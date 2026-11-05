#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "../../Effects/IEngineComputeOutput.h"

using namespace ShaderLab::Graph;

// Deferred analysis commit (EvaluatorOptions::deferredAnalysisCommit). A
// compute producer's bound consumers render with a committed record -- the
// node's analysisOutput.fields, which its lane-3 texture matches texel for
// texel. Every dispatch copies its values back without waiting; when they
// land, each field is judged against its committed value under its policy,
// and only the fields that changed are written, to the record and to their
// own texels, waking only the consumers bound to them.
namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        using TextureCopy = Rendering::D3D11ComputeRunner::TextureCopy;

        // Each field's first texel and texel count in the analysis row.
        struct FieldLayout
        {
            std::vector<uint32_t> first;
            std::vector<uint32_t> count;
            uint32_t total{ 0 };
        };

        FieldLayout LayoutOf(const CustomEffectDefinition& def)
        {
            FieldLayout layout;
            for (const auto& field : def.analysisFields)
            {
                layout.first.push_back(layout.total);
                layout.count.push_back(field.pixelCount());
                layout.total += field.pixelCount();
            }
            return layout;
        }

        // A keep mask over the row's texels: bit t set for every texel of a
        // field `keep` accepts. Never empty, so it always reads as a mask.
        template <class Pred>
        std::vector<uint32_t> TexelMask(const FieldLayout& layout, Pred&& keep)
        {
            std::vector<uint32_t> mask((std::max)((layout.total + 31) / 32, 1u), 0u);
            for (size_t f = 0; f < layout.first.size(); ++f)
            {
                if (!keep(f)) continue;
                for (uint32_t t = layout.first[f]; t < layout.first[f] + layout.count[f]; ++t)
                    mask[t >> 5] |= 1u << (t & 31);
            }
            return mask;
        }

        std::optional<uint32_t> FieldIndexOf(const CustomEffectDefinition& def, std::wstring_view name)
        {
            for (uint32_t f = 0; f < def.analysisFields.size(); ++f)
                if (def.analysisFields[f].name == name) return f;
            return std::nullopt;
        }

        // Calls fn(fieldName) for each field of `producerId` the binding reads.
        template <class Fn>
        void ForEachFieldRead(const PropertyBinding& binding, uint32_t producerId, Fn&& fn)
        {
            if (binding.wholeArray)
            {
                if (binding.wholeArraySourceNodeId == producerId) fn(binding.wholeArraySourceFieldName);
                return;
            }
            for (const auto& source : binding.sources)
                if (source && source->sourceNodeId == producerId) fn(source->sourceFieldName);
        }

        bool ReadsAnyField(const EffectNode& consumer, uint32_t producerId, const std::unordered_set<std::wstring>& fields)
        {
            for (const auto& [property, binding] : consumer.propertyBindings)
            {
                bool reads = false;
                ForEachFieldRead(binding, producerId, [&](const std::wstring& name) { reads |= fields.contains(name); });
                if (reads) return true;
            }
            return false;
        }

        bool SameBits(const AnalysisFieldValue& a, const AnalysisFieldValue& b)
        {
            return a.type == b.type &&
                   std::memcmp(a.components.data(), b.components.data(), sizeof(a.components)) == 0 &&
                   a.arrayData.size() == b.arrayData.size() &&
                   (a.arrayData.empty() ||
                    std::memcmp(a.arrayData.data(), b.arrayData.data(), a.arrayData.size() * sizeof(float)) == 0);
        }

        // A host-committed scalar: the value in x, as one texel holds it.
        AnalysisFieldValue HostValue(const AnalysisFieldDescriptor& desc, float value)
        {
            AnalysisFieldValue field;
            field.name = desc.name;
            field.type = desc.type;
            field.components = { value, 0.0f, 0.0f, 0.0f };
            return field;
        }

        void EnsureSized(DeferredCommit& rec, size_t fieldCount)
        {
            rec.known.resize(fieldCount, 0);
            rec.hostOwned.resize(fieldCount, 0);
            rec.hostOwnedValue.resize(fieldCount, 0.0f);
            rec.hostWrite.resize(fieldCount, 0);
        }
    }

    // -----------------------------------------------------------------------
    // Policies
    // -----------------------------------------------------------------------

    EvaluatorCore::FieldPolicies EvaluatorCore::PoliciesOf(const GraphModel& graph, const EffectNode& producer) const
    {
        FieldPolicies policies;
        if (!producer.customEffect.has_value()) return policies;
        const auto& def = *producer.customEffect;
        const size_t fieldCount = def.analysisFields.size();
        policies.policy.assign(fieldCount, CommitPolicy::Exact);
        policies.bound.assign(fieldCount, 0);
        policies.conflicted.assign(fieldCount, 0);
        for (const auto& consumer : graph.Nodes())
        {
            for (const auto& [property, binding] : consumer.propertyBindings)
            {
                ForEachFieldRead(binding, producer.id, [&](const std::wstring& name)
                {
                    const auto f = FieldIndexOf(def, name);
                    if (!f) return;
                    if (policies.bound[*f] && policies.policy[*f] != binding.commitPolicy)
                        policies.conflicted[*f] = 1;
                    policies.bound[*f] = 1;
                    policies.policy[*f] = binding.commitPolicy;
                });
            }
        }
        for (size_t f = 0; f < fieldCount; ++f)
            if (policies.conflicted[f]) policies.policy[f] = CommitPolicy::Exact;
        return policies;
    }

    const EvaluatorCore::FieldPolicies& EvaluatorCore::CachedPoliciesOf(const GraphModel& graph,
                                                                        const EffectNode& producer) const
    {
        if (const auto it = m_policyCache.find(producer.id); it != m_policyCache.end()) return it->second;
        return m_policyCache.emplace(producer.id, PoliciesOf(graph, producer)).first->second;
    }

    std::optional<CommitPolicy> EvaluatorCore::FieldCommitPolicy(const GraphModel& graph, uint32_t nodeId,
                                                                 uint32_t fieldIndex) const
    {
        const auto options = Options();
        if (!options.deferredAnalysisCommit || !options.skipUnneededCpuReadback) return CommitPolicy::Immediate;
        const auto* node = graph.FindNode(nodeId);
        if (!node || !node->customEffect.has_value() || fieldIndex >= node->customEffect->analysisFields.size())
            return CommitPolicy::Exact;
        const FieldPolicies policies = PoliciesOf(graph, *node);
        if (policies.conflicted[fieldIndex]) return std::nullopt;
        return policies.policy[fieldIndex];
    }

    std::wstring EvaluatorCore::CommitPolicyConflict(const EffectNode& consumer, const GraphModel& graph) const
    {
        if (!DeferredCommitOn() || consumer.propertyBindings.empty()) return {};
        for (const auto& [property, binding] : consumer.propertyBindings)
        {
            std::wstring conflict;
            binding.ForEachSourceNode([&](uint32_t producerId)
            {
                if (!conflict.empty()) return;
                const auto* producer = graph.FindNode(producerId);
                if (!producer || !producer->customEffect.has_value()) return;
                const FieldPolicies& policies = CachedPoliciesOf(graph, *producer);
                ForEachFieldRead(binding, producerId, [&](const std::wstring& name)
                {
                    const auto f = FieldIndexOf(*producer->customEffect, name);
                    if (conflict.empty() && f && policies.conflicted[*f])
                        conflict = std::format(L"binding '{}': the bindings of field '{}' of node {} disagree on "
                                               L"its commit policy", property, name, producerId);
                });
            });
            if (!conflict.empty()) return conflict;
        }
        return {};
    }

    // -----------------------------------------------------------------------
    // Waking consumers
    // -----------------------------------------------------------------------

    void EvaluatorCore::WakeFieldConsumers(GraphModel& graph, uint32_t producerId,
                                           const std::unordered_set<std::wstring>& fields)
    {
        if (fields.empty()) return;
        std::vector<uint32_t> consumers;
        for (const auto& consumer : graph.Nodes())
            if (ReadsAnyField(consumer, producerId, fields)) consumers.push_back(consumer.id);
        for (uint32_t consumerId : consumers)
        {
            if (auto* consumer = graph.FindNode(consumerId))
            {
                consumer->MarkDirty();
                graph.MarkDownstreamDirty(consumerId);
            }
        }
    }

    void EvaluatorCore::SyncFieldConsumers(GraphModel& graph, uint32_t producerId,
                                           const std::unordered_set<std::wstring>& fields)
    {
        if (fields.empty()) return;
        std::vector<uint32_t> consumers;
        for (const auto& consumer : graph.Nodes())
            if (ReadsAnyField(consumer, producerId, fields)) consumers.push_back(consumer.id);
        for (uint32_t consumerId : consumers)
        {
            auto* consumer = graph.FindNode(consumerId);
            if (!consumer || consumer->IsDirty()) continue;   // its visit resolves the record anyway
            // Served on the GPU, it rendered the texel the record now holds:
            // take the value without re-rendering. Otherwise it rendered the
            // record's old value from its cbuffer, and has to run again.
            const GpuBindingPlan plan = PlanGpuBindings(*consumer, graph, m_frameOptions.gpuBindings,
                [this](uint32_t id) { return BridgeFor(id) != nullptr; });
            bool gpuServed = true;
            for (const auto& [property, binding] : consumer->propertyBindings)
            {
                if (!binding.ReadsFrom(producerId)) continue;
                const auto* entry = plan.Find(property);
                if (!entry || entry->mode == Effects::GpuBindingMode::CBuffer ||
                    AppliedBindingMode(consumer->id, entry->gpuBindableIndex) != entry->mode)
                    gpuServed = false;
            }
            if (gpuServed)
            {
                PropertyMap effectiveProps;
                ResolveAndWriteBackBindings(*consumer, graph, effectiveProps);
            }
            else
            {
                consumer->MarkDirty();
                graph.MarkDownstreamDirty(consumerId);
            }
        }
    }

    void EvaluatorCore::CountCommit(uint32_t producerId)
    {
        auto& runtime = m_nodes[producerId];
        if (m_pass.counted.insert(producerId).second) ++runtime.outputChangeCount;
        runtime.pendingRegion = DirtyRegion::Whole();
    }

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    void EvaluatorCore::PrepareDeferredDispatch(const EffectNode& node, NodeRuntime& runtime,
                                                Effects::CustomComputeBridgeEffect* bridge,
                                                const FieldPolicies& policies, bool wholeInput)
    {
        const FieldLayout layout = LayoutOf(*node.customEffect);
        const bool anyHosted = policies.Any(CommitPolicy::Hosted);
        const auto notHosted = [&](size_t f) { return policies.policy[f] != CommitPolicy::Hosted; };

        // An empty texture takes every texel (the first fill); a whole
        // dispatch every texel but the Hosted ones, which move only on a
        // commit; a dispatch over rects only the Immediate ones, whose
        // consumers see each dispatch's values at once.
        if (!bridge->AnalysisTextureFilled() || runtime.deferred.bufferGeneration != bridge->BufferGeneration())
            bridge->SetTextureCopy(TextureCopy::All);
        else if (wholeInput)
            anyHosted ? bridge->SetTextureCopy(TextureCopy::Masked, TexelMask(layout, notHosted))
                      : bridge->SetTextureCopy(TextureCopy::All);
        else if (policies.Any(CommitPolicy::Immediate))
            bridge->SetTextureCopy(TextureCopy::Masked, TexelMask(layout, [&](size_t f)
                { return policies.bound[f] && policies.policy[f] == CommitPolicy::Immediate; }));
        else
            bridge->SetTextureCopy(TextureCopy::None);
    }

    void EvaluatorCore::FinishDeferredDispatch(GraphModel& graph, EffectNode& node, NodeRuntime& runtime,
                                               Effects::CustomComputeBridgeEffect* bridge,
                                               const FieldPolicies& policies, bool wholeInput, bool filledBefore,
                                               uint64_t generationBefore, uint64_t seqBefore,
                                               const std::vector<float>* blockingFloats)
    {
        auto& rec = runtime.deferred;
        const auto& def = *node.customEffect;
        const size_t fieldCount = def.analysisFields.size();

        // New buffers (the first dispatch, a new result count): what the
        // record said about the texture no longer holds. A host commit made
        // before any seed still stands.
        if (bridge->BufferGeneration() != rec.bufferGeneration)
        {
            DeferredCommit fresh;
            fresh.bufferGeneration = bridge->BufferGeneration();
            fresh.dispatches = rec.dispatches;
            fresh.landed = std::move(rec.landed);
            fresh.landedDispatch = rec.landedDispatch;
            fresh.landedFrame = rec.landedFrame;
            fresh.hostCommits = std::move(rec.hostCommits);
            if (!rec.seeded)
            {
                fresh.hostOwned = std::move(rec.hostOwned);
                fresh.hostOwnedValue = std::move(rec.hostOwnedValue);
            }
            rec = std::move(fresh);
        }
        EnsureSized(rec, fieldCount);
        const uint64_t dispatch = ++rec.dispatches;

        const bool filledNow = bridge->AnalysisTextureFilled();
        const bool firstFill = filledNow && (!filledBefore || generationBefore != bridge->BufferGeneration());
        const bool copyFailed = bridge->TextureCopyFailed();
        const FieldLayout layout = LayoutOf(def);

        std::unordered_set<std::wstring> wake;
        if (firstFill)
        {
            // Every texel holds this dispatch's values; its readback seeds the
            // record. A Hosted field keeps what the host committed: before the
            // seed its host-owned value, and once seeded (from readbacks taken
            // while no consumer wanted the texture) its committed record.
            rec.firstFill = rec.stamp = dispatch;
            std::fill(rec.known.begin(), rec.known.end(), uint8_t{ 0 });
            const auto& fields = node.analysisOutput.fields;
            for (size_t f = 0; f < fieldCount; ++f)
            {
                if (rec.seeded && policies.policy[f] == CommitPolicy::Hosted && f < fields.size())
                {
                    const auto row = PackAnalysisFields({ fields[f] });
                    if (row.size() >= static_cast<size_t>(layout.count[f]) * 4)
                        bridge->WriteAnalysisTexels(layout.first[f], row.data(), layout.count[f]);
                    rec.known[f] = 1;
                }
                else if (!rec.seeded && rec.hostOwned[f])
                {
                    const float texel[4] = { rec.hostOwnedValue[f], 0.0f, 0.0f, 0.0f };
                    bridge->WriteAnalysisTexels(layout.first[f], texel, 1);
                }
            }
            ++m_deferredStats.wholeDispatches;
            ++m_bindingGate.whole;
            // A new texture is a new input for every consumer.
            MarkBindingConsumersDirty(graph, node.id);
        }
        else if (copyFailed)
        {
            // The copy pass did not run: the texture holds what it held, and
            // the record still describes it. The landing judges every field
            // against the record and writes the texels of those that changed.
            ++m_deferredStats.copyFailures;
            ++m_bindingGate.deferred;
        }
        else if (wholeInput)
        {
            // The stamp rule: the copy pass wrote every non-Hosted texel, so
            // their records are unknown until this dispatch's readback lands.
            rec.stamp = dispatch;
            for (size_t f = 0; f < fieldCount; ++f)
            {
                if (policies.policy[f] == CommitPolicy::Hosted) continue;
                rec.known[f] = 0;
                if (policies.bound[f]) wake.insert(def.analysisFields[f].name);
            }
            ++m_deferredStats.wholeDispatches;
            ++m_bindingGate.whole;
        }
        else
        {
            for (size_t f = 0; f < fieldCount; ++f)
            {
                if (!policies.bound[f] || policies.policy[f] != CommitPolicy::Immediate) continue;
                rec.known[f] = 0;
                wake.insert(def.analysisFields[f].name);
            }
            ++m_deferredStats.rectsDispatches;
            ++m_bindingGate.deferred;
        }
        WakeFieldConsumers(graph, node.id, wake);

        if (bridge->LastScheduledReadback() != seqBefore && bridge->HasPendingReadback())
            rec.inFlight.push_back({ bridge->LastScheduledReadback(), dispatch, m_frame, copyFailed });
        if (blockingFloats && !blockingFloats->empty())
            LandDeferred(graph, node, runtime, { 0, dispatch, m_frame, copyFailed }, *blockingFloats);
    }

    // -----------------------------------------------------------------------
    // Landing
    // -----------------------------------------------------------------------

    bool EvaluatorCore::LandDeferred(GraphModel& graph, EffectNode& node, NodeRuntime& runtime,
                                     const DeferredCommit::InFlight& slot, const std::vector<float>& floats)
    {
        const uint64_t dispatch = slot.dispatch;
        const uint64_t scheduledFrame = slot.frame;
        auto& rec = runtime.deferred;
        const auto& def = *node.customEffect;
        const size_t fieldCount = def.analysisFields.size();
        EnsureSized(rec, fieldCount);
        auto landed = UnpackAnalysisFields(def.analysisFields, floats.data(), floats.size());
        ++m_deferredStats.landed;
        if (dispatch >= rec.landedDispatch)
        {
            rec.landed = landed;
            rec.landedDispatch = dispatch;
            rec.landedFrame = scheduledFrame;
        }
        // Older than the texture's first fill: it describes another texture.
        // Unseeded and older than the stamp: its values are not the ones the
        // texture holds, so the stamp's own slot (or a later one) seeds.
        if (dispatch < rec.firstFill || (!rec.seeded && dispatch < rec.stamp))
        {
            ++m_deferredStats.discarded;
            return false;
        }

        auto* bridge = runtime.instance.bridgeImpl;
        const bool filled = bridge && bridge->AnalysisTextureFilled();
        const FieldLayout layout = LayoutOf(def);
        const FieldPolicies& policies = CachedPoliciesOf(graph, node);
        auto& fields = node.analysisOutput.fields;
        // Fields whose texels take the landed values, written after the loop
        // one run of adjacent fields per call (each is an immediate-context
        // UpdateSubresource).
        std::vector<uint8_t> write(fieldCount, 0);
        const auto writeTexels = [&](size_t f) { write[f] = 1; };

        // The seed: the first readback since the first fill gives the record
        // its layout, and every field a value but the ones the host owns,
        // which take the host's value and are then judged like any other.
        const bool seeding = !rec.seeded;
        std::vector<uint8_t> hostOwned(fieldCount, 0);
        // Fields the host wrote (SetFloats) after this slot's dispatch: their
        // record and texel hold the host's values, which this slot predates.
        std::vector<uint8_t> hostWritten(fieldCount, 0);
        for (size_t f = 0; f < fieldCount; ++f)
            hostWritten[f] = rec.hostWrite[f] >= dispatch && (!seeding || fields.size() == fieldCount);
        if (seeding)
        {
            node.analysisOutput.type = AnalysisOutputType::Typed;
            auto seed = landed;
            for (size_t f = 0; f < fieldCount; ++f)
                if (hostWritten[f]) seed[f] = fields[f];
            fields = std::move(seed);
            for (size_t f = 0; f < fieldCount; ++f)
                if (rec.hostOwned[f]) fields[f] = HostValue(def.analysisFields[f], rec.hostOwnedValue[f]);
            hostOwned = std::exchange(rec.hostOwned, std::vector<uint8_t>(fieldCount, 0));
            std::fill(rec.hostOwnedValue.begin(), rec.hostOwnedValue.end(), 0.0f);
            rec.seeded = true;
            ++m_deferredStats.seeds;
        }

        std::unordered_set<std::wstring> woke, silent;
        bool stale = false;
        for (size_t f = 0; f < fieldCount; ++f)
        {
            const auto& name = def.analysisFields[f].name;
            const CommitPolicy policy = policies.policy[f];
            if (hostOwned[f]) continue;
            if (hostWritten[f])
            {
                ++m_deferredStats.hostWritesKept;
                continue;
            }
            if (policy == CommitPolicy::Hosted)
            {
                // Seeded once; afterwards only CommitFields writes it. A seed
                // from a later slot than the first fill finds its texel older.
                if (!seeding) continue;
                rec.known[f] = 1;
                if (dispatch == rec.firstFill)
                    silent.insert(name);
                else
                {
                    writeTexels(f);
                    woke.insert(name);
                    ++m_deferredStats.hostedSeeds;
                }
                continue;
            }
            // Older than the last whole dispatch, whose copy pass the texel holds.
            if (dispatch < rec.stamp)
            {
                stale = true;
                continue;
            }
            if (policy == CommitPolicy::Immediate && !slot.copyFailed)
            {
                // Its texel was copied by the dispatch itself.
                fields[f] = landed[f];
                rec.known[f] = 1;
                silent.insert(name);
                continue;
            }
            if (!rec.known[f] && dispatch == rec.stamp)
            {
                // The whole dispatch's own values: already in the texel.
                fields[f] = landed[f];
                rec.known[f] = 1;
                silent.insert(name);
                ++m_deferredStats.stamps;
                continue;
            }
            bool changed = !rec.known[f] || seeding;
            if (!changed && !SameBits(fields[f], landed[f]))
            {
                const float threshold = def.analysisFields[f].changeThreshold;
                const bool scalar = def.analysisFields[f].type == AnalysisFieldType::Float;
                if (policy == CommitPolicy::Threshold && scalar && threshold > 0.0f &&
                    std::fabs(landed[f].components[0] - fields[f].components[0]) <= threshold)
                    ++m_deferredStats.subThreshold;
                else
                    changed = true;
            }
            if (!changed) continue;
            fields[f] = landed[f];
            rec.known[f] = 1;
            writeTexels(f);
            // Nobody reads it: recorded, so record and texture agree, waking no one.
            if (!policies.bound[f]) continue;
            woke.insert(name);
            ++(policy == CommitPolicy::Threshold ? m_deferredStats.thresholdCommits : m_deferredStats.exactCommits);
        }
        if (stale) ++m_deferredStats.discarded;

        for (size_t f = 0; f < fieldCount && filled;)
        {
            if (!write[f])
            {
                ++f;
                continue;
            }
            size_t last = f;
            while (last + 1 < fieldCount && write[last + 1]) ++last;
            const uint32_t first = layout.first[f];
            const uint32_t count = layout.first[last] + layout.count[last] - first;
            if (floats.size() >= (static_cast<size_t>(first) + count) * 4)
                bridge->WriteAnalysisTexels(first, floats.data() + static_cast<size_t>(first) * 4, count);
            f = last + 1;
        }

        SyncFieldConsumers(graph, node.id, silent);
        if (woke.empty()) return false;
        WakeFieldConsumers(graph, node.id, woke);
        CountCommit(node.id);
        ++m_deferredStats.commitLandings;
        // Between frames (PollReadbacks), the commit is drawn by the next one.
        const uint64_t now = m_frameOpen ? m_frame : m_frame + 1;
        const uint64_t distance = now >= scheduledFrame ? now - scheduledFrame : 0;
        m_deferredStats.landingFramesSum += distance;
        m_deferredStats.landingFramesMax = (std::max)(m_deferredStats.landingFramesMax, distance);
        return true;
    }

    void EvaluatorCore::ApplyHostCommits(GraphModel& graph)
    {
        for (auto& [id, runtime] : m_nodes)
        {
            auto& rec = runtime.deferred;
            if (rec.hostCommits.empty()) continue;
            const auto commits = std::exchange(rec.hostCommits, {});
            auto* node = graph.FindNode(id);
            if (!node || !node->customEffect.has_value()) continue;
            const auto& def = *node->customEffect;
            EnsureSized(rec, def.analysisFields.size());
            auto* bridge = runtime.instance.bridgeImpl;
            const bool filled = bridge && bridge->AnalysisTextureFilled();
            const FieldLayout layout = LayoutOf(def);
            auto& fields = node->analysisOutput.fields;

            std::unordered_set<std::wstring> woke;
            for (const auto& [f, value] : commits)
            {
                if (f >= def.analysisFields.size()) continue;
                const AnalysisFieldValue committed = HostValue(def.analysisFields[f], value);
                if (rec.seeded)
                {
                    if (f < fields.size() && SameBits(fields[f], committed)) continue;   // holds already
                    if (f < fields.size()) fields[f] = committed;
                }
                else
                {
                    // Before the seed: the field is the host's, and the seed skips it.
                    if (rec.hostOwned[f] && rec.hostOwnedValue[f] == value) continue;
                    rec.hostOwned[f] = 1;
                    rec.hostOwnedValue[f] = value;
                }
                rec.known[f] = 1;
                if (filled)
                {
                    const float texel[4] = { value, 0.0f, 0.0f, 0.0f };
                    bridge->WriteAnalysisTexels(layout.first[f], texel, 1);
                }
                woke.insert(def.analysisFields[f].name);
                ++m_deferredStats.hostedCommits;
                // Measured from the dispatch whose landed values the host read.
                const uint64_t now = m_frameOpen ? m_frame : m_frame + 1;
                const uint64_t distance = rec.landedDispatch && now >= rec.landedFrame ? now - rec.landedFrame : 0;
                m_deferredStats.hostedFramesSum += distance;
                m_deferredStats.hostedFramesMax = (std::max)(m_deferredStats.hostedFramesMax, distance);
            }
            if (woke.empty()) continue;
            WakeFieldConsumers(graph, id, woke);
            CountCommit(id);
        }
    }

    bool EvaluatorCore::PollReadbacks(GraphModel& graph, bool block)
    {
        // Between passes: a commit here is counted on its own, and the graph
        // may have been edited since the last.
        m_pass.counted.clear();
        m_policyCache.clear();
        bool any = false;
        if (block)
        {
            for (auto& [id, runtime] : m_nodes)
            {
                if (!runtime.asyncReadbackPending || runtime.deferred.dispatches == 0) continue;
                auto* node = graph.FindNode(id);
                auto* bridge = runtime.instance.bridgeImpl;
                if (!node || !node->customEffect.has_value() || !bridge) continue;
                std::vector<float> floats;
                uint64_t seq = 0;
                if (bridge->CollectAnalysisReadbackBlocking(floats, &seq) && !floats.empty())
                {
                    ++m_readbackStats.freshReadbacks;
                    ++m_deferredStats.blockingDrains;
                    auto& inFlight = runtime.deferred.inFlight;
                    const auto slot = std::ranges::find(inFlight, seq, &DeferredCommit::InFlight::seq);
                    if (slot != inFlight.end())
                        any |= LandDeferred(graph, *node, runtime, *slot, floats);
                }
                runtime.deferred.inFlight.clear();
                runtime.asyncReadbackPending = bridge->HasPendingReadback();
            }
        }
        if (HasPendingReadbacks())
            any |= PollAsyncReadbacks(graph);
        return any;
    }

    bool EvaluatorCore::LandedValues(uint32_t nodeId, std::vector<AnalysisFieldValue>& values) const
    {
        const auto* runtime = FindRuntime(nodeId);
        if (!runtime || runtime->deferred.landedDispatch == 0) return false;
        values = runtime->deferred.landed;
        return true;
    }

    bool EvaluatorCore::CommittedValues(const GraphModel& graph, uint32_t nodeId,
                                        std::vector<AnalysisFieldValue>& values, bool& pending) const
    {
        pending = false;
        const auto* runtime = FindRuntime(nodeId);
        if (!runtime || !runtime->deferred.seeded) return false;
        const auto* node = graph.FindNode(nodeId);
        if (!node || !node->customEffect) return false;
        values = node->analysisOutput.fields;
        // A bound field whose texel a dispatch wrote before its readback
        // landed (the stamp rule, or an Immediate field): consumers render
        // with values the record does not hold yet.
        const auto& known = runtime->deferred.known;
        const FieldPolicies& policies = CachedPoliciesOf(graph, *node);
        for (size_t f = 0; f < known.size() && f < policies.bound.size(); ++f)
            if (policies.bound[f] && policies.policy[f] != CommitPolicy::Hosted && !known[f]) pending = true;
        return true;
    }

    HRESULT EvaluatorCore::ReadAnalysisTexture(uint32_t nodeId, std::vector<float>& float4s) const
    {
        float4s.clear();
        auto* bridge = BridgeFor(nodeId);
        if (!bridge || !bridge->AnalysisTextureFilled()) return E_NOT_VALID_STATE;
        winrt::com_ptr<Effects::IEngineComputeTexture> computeTexture;
        if (FAILED(bridge->QueryInterface(__uuidof(Effects::IEngineComputeTexture), computeTexture.put_void())))
            return E_NOT_VALID_STATE;
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(computeTexture->GetAnalysisTexture(texture.put())) || !texture) return E_NOT_VALID_STATE;

        winrt::com_ptr<ID3D11Device> device;
        texture->GetDevice(device.put());
        winrt::com_ptr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(immediate.put());
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        if (HRESULT hr = device->CreateTexture2D(&desc, nullptr, staging.put()); FAILED(hr)) return hr;
        immediate->CopyResource(staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (HRESULT hr = immediate->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped); FAILED(hr)) return hr;
        const auto* row = static_cast<const float*>(mapped.pData);
        float4s.assign(row, row + static_cast<size_t>(desc.Width) * 4);
        immediate->Unmap(staging.get(), 0);
        return S_OK;
    }

    HRESULT EvaluatorCore::WriteDeferredResults(const GraphModel& graph, const EffectNode& node, NodeRuntime& runtime,
                                                Effects::CustomComputeBridgeEffect* bridge, uint32_t firstElement,
                                                const float* float4s, uint32_t elementCount)
    {
        // The buffer takes every value; the copy pass is not run, since it
        // would also publish whatever the buffer holds for the other fields.
        if (HRESULT hr = bridge->WriteResults(firstElement, float4s, elementCount, false); FAILED(hr)) return hr;
        auto& rec = runtime.deferred;
        const auto& def = *node.customEffect;
        EnsureSized(rec, def.analysisFields.size());
        const FieldLayout layout = LayoutOf(def);
        const FieldPolicies policies = PoliciesOf(graph, node);
        const bool filled = bridge->AnalysisTextureFilled();
        const uint64_t end = uint64_t{ firstElement } + elementCount;
        for (size_t f = 0; f < def.analysisFields.size(); ++f)
        {
            // A Hosted field moves only on CommitFields.
            if (policies.policy[f] == CommitPolicy::Hosted) continue;
            const uint64_t lo = (std::max)(uint64_t{ layout.first[f] }, uint64_t{ firstElement });
            const uint64_t hi = (std::min)(uint64_t{ layout.first[f] } + layout.count[f], end);
            if (lo >= hi) continue;
            // Readbacks of this dispatch or older, still in flight, predate
            // the write: their landing (the seed included) leaves the field.
            rec.hostWrite[f] = rec.dispatches;
            if (!filled) continue;
            const HRESULT hr = bridge->WriteAnalysisTexels(static_cast<uint32_t>(lo), float4s + (lo - firstElement) * 4,
                                                           static_cast<uint32_t>(hi - lo));
            if (FAILED(hr)) return hr;
            rec.known[f] = 1;
        }
        return S_OK;
    }

    HRESULT EvaluatorCore::CommitFields(const GraphModel& graph, uint32_t nodeId, const uint32_t* fieldIndices,
                                        const float* values, uint32_t count)
    {
        const auto options = Options();
        if (!options.deferredAnalysisCommit || !options.skipUnneededCpuReadback) return E_NOT_VALID_STATE;
        if (count == 0) return S_FALSE;
        if (!fieldIndices || !values) return E_POINTER;
        const auto* node = graph.FindNode(nodeId);
        if (!node || !node->customEffect.has_value() ||
            node->customEffect->shaderType != CustomShaderType::D3D11ComputeShader)
            return E_INVALIDARG;
        const auto& def = *node->customEffect;
        const FieldPolicies policies = PoliciesOf(graph, *node);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t f = fieldIndices[i];
            if (f >= def.analysisFields.size() || !policies.bound[f] || policies.conflicted[f] ||
                policies.policy[f] != CommitPolicy::Hosted || def.analysisFields[f].type != AnalysisFieldType::Float ||
                !std::isfinite(values[i]))
                return E_INVALIDARG;
        }
        auto& commits = m_nodes[nodeId].deferred.hostCommits;
        for (uint32_t i = 0; i < count; ++i)
            commits.emplace_back(fieldIndices[i], values[i]);
        return S_OK;
    }
}
