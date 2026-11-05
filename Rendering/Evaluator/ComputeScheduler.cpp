#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "ComputeDispatch.h"
#include "../D2DHelpers.h"
#include "../../Effects/IEngineComputeOutput.h"
#include "../../Effects/Performance.h"
#include "../../Effects/ShaderVariants.h"

using namespace ShaderLab::Graph;

// D3D11 compute nodes: Evaluate queues them (QueueD3D11Compute) and
// ProcessDeferredCompute dispatches them through their bridge effect once
// every D2D effect of the pass exists. It runs outside any draw session and
// opens its own only to render an input the shader cannot read in place.
namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        bool IsComputeNode(const EffectNode& node)
        {
            return node.customEffect.has_value() &&
                   node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;
        }

        bool PinWired(const std::vector<const EffectEdge*>& inputs, uint32_t pin)
        {
            return std::any_of(inputs.begin(), inputs.end(),
                [pin](const auto* edge) { return edge->destPin == pin; });
        }
    }

    void EvaluatorCore::QueueD3D11Compute(GraphModel& graph, EffectNode& node, ID2D1DeviceContext5* dc)
    {
        const auto& def = node.customEffect.value();
        if (!node.propertyBindings.empty())
        {
            PropertyMap effectiveProps;
            if (ResolveAndWriteBackBindings(node, graph, effectiveProps))
            {
                // A parameter of its own moved: every pixel reads it.
                node.MarkDirty();
                m_nodes[node.id].ownStateChanged = true;
                MarkComputeChanged(node);
            }
        }

        // Every upstream image in pin order (inputNames order); the bridge
        // binds them at t0..t(N-1). A slot with nothing connected stays null
        // and the bridge reports it.
        auto inputs = graph.GetInputEdges(node.id);
        std::vector<winrt::com_ptr<ID2D1Image>> inputImages;
        {
            uint32_t maxPin = 0;
            for (const auto* e : inputs)
                if (e->destPin >= maxPin) maxPin = e->destPin + 1;
            const uint32_t declaredCount = static_cast<uint32_t>(def.inputNames.size());
            uint32_t totalSlots = (std::max)(maxPin, declaredCount);
            if (totalSlots == 0 && !inputs.empty()) totalSlots = 1;
            inputImages.assign(totalSlots, {});
            for (const auto* e : inputs)
            {
                auto* srcNode = graph.FindNode(e->sourceNodeId);
                if (srcNode && srcNode->cachedOutput && e->destPin < inputImages.size())
                    inputImages[e->destPin].copy_from(srcNode->cachedOutput);
            }
        }
        auto placeholder = [&](winrt::com_ptr<ID2D1Image>& slot)
        {
            EnsureAnalysisDummy(dc);
            if (m_analysisDummyBitmap)
                slot.copy_from(static_cast<ID2D1Image*>(m_analysisDummyBitmap.get()));
        };
        // A GENERATOR declares no image inputs: its output is a function of
        // its parameters alone. The compute path assumes an input 0 throughout,
        // so it gets the placeholder -- which is never dirty, so the generator
        // re-dispatches only when one of its own properties moves.
        if (def.inputNames.empty() && inputImages.empty())
        {
            EnsureAnalysisDummy(dc);
            if (m_analysisDummyBitmap)
                placeholder(inputImages.emplace_back());
        }
        // An unwired lookup input gets the placeholder too, as on the pixel
        // path: its alpha of 0 is how the shader reads "no table". A wired pin
        // whose producer has no output yet stays empty.
        if (const uint32_t lookupCount = def.lookupInputCount; lookupCount > 0)
        {
            const uint32_t declared = static_cast<uint32_t>(def.inputNames.size());
            const uint32_t firstLookup = declared - (std::min)(lookupCount, declared);
            for (uint32_t pin = firstLookup; pin < declared && pin < inputImages.size(); ++pin)
                if (!PinWired(inputs, pin)) placeholder(inputImages[pin]);
        }
        // So does every unwired pin of a variadic node; its InputMask says
        // which pins are real.
        if (def.variadicInputs)
        {
            for (uint32_t pin = 0; pin < inputImages.size(); ++pin)
                if (!PinWired(inputs, pin)) placeholder(inputImages[pin]);
        }

        ID2D1Image* primaryInput = inputImages.empty() ? nullptr : inputImages[0].get();
        const bool hasImageOutput = !node.outputPins.empty();
        // A compute consumer must redispatch when one of its binding SOURCES
        // is queued this frame: its dispatch reads that source's analysis SRV,
        // which is about to change. Otherwise the SRV holds last frame's
        // values and dirty-gating is safe (redispatching every compute node
        // every frame defeated output caching downstream).
        const bool bindingSourceQueued = AnyBindingSource(node,
            [this](uint32_t sourceNodeId) { return m_computeQueue.queued.contains(sourceNodeId); });
        // An analysis-only node with no fields has produced nothing yet,
        // unless its results are on the GPU only.
        const NodeRuntime* runtime = FindRuntime(node.id);
        const bool gpuOnlyResults = runtime && runtime->cpuFieldsStale;
        // Deferred commit: a lane-3 binding new on a clean producer is served
        // from its filled texture, or by one whole dispatch this frame --
        // never by a CPU readback.
        bool laneThreeNew = false;
        if (DeferredCommitOn() && !node.IsDirty() && !m_frozen && !AnalysisBitmap(node.id))
        {
            for (const auto& consumer : graph.Nodes())
            {
                if (laneThreeNew) break;
                if (!consumer.needed || GpuLaneFor(consumer) != Effects::GpuBindingMode::Texture ||
                    !AnyBindingSource(consumer, [&](uint32_t id) { return id == node.id; }))
                    continue;
                const GpuBindingPlan plan = PlanGpuBindings(consumer, graph, m_frameOptions.gpuBindings,
                    [this](uint32_t producerId) { return BridgeFor(producerId) != nullptr; });
                laneThreeNew = std::ranges::any_of(plan.entries, [&](const GpuBindingEntry& entry)
                    { return entry.producerId == node.id && entry.mode == Effects::GpuBindingMode::Texture; });
            }
            auto* bridge = BridgeFor(node.id);
            if (laneThreeNew && bridge && bridge->AnalysisTextureFilled() && EnsureAnalysisBitmap(node.id, dc))
                laneThreeNew = false;
            if (laneThreeNew)
                m_nodes[node.id].ownStateChanged = true;
        }
        // An input change no dispatch has consumed yet (a dropped queue, a
        // failed dispatch, a frame the node could not queue) queues it again
        // with nothing new, unless the node is in error: RetryNode, or a
        // change that clears the error, re-arms it.
        const bool regionPending = runtime && !runtime->undispatchedRegion.IsNone() && node.runtimeError.empty();
        const bool needsCompute = node.IsDirty() || laneThreeNew || regionPending ||
            bindingSourceQueued ||
            (hasImageOutput && !node.cachedOutput) ||
            (!hasImageOutput && node.analysisOutput.fields.empty() && !gpuOnlyResults);
        // A pin fed by a compute node queued this frame has no image yet;
        // ProcessDeferredCompute resolves it after that producer dispatches.
        const bool inputFromQueuedProducer = std::any_of(inputs.begin(), inputs.end(),
            [this](const auto* edge) { return m_computeQueue.queued.contains(edge->sourceNodeId); });
        // Queued at most once per frame: the dispatch reads the node's state
        // when it runs, so one entry is always current.
        const bool queues = (primaryInput || inputFromQueuedProducer) && needsCompute && !m_frozen &&
                            !m_computeQueue.queued.contains(node.id);

        // What the next dispatch consumes: kept here because the dirty state
        // is cleared below, queued or not, and a queued dispatch can still be
        // dropped or fail. Unknown is whole.
        if (!m_frozen)
        {
            NodeRuntime& pending = m_nodes[node.id];
            if (bindingSourceQueued) pending.ownStateChanged = true;
            DirtyRegion input;
            if (node.IsDirty())
                input = m_frameOptions.dirtyRegions ? node.Region() : DirtyRegion::Whole();
            if (pending.ownStateChanged || (queues && input.IsNone() && pending.undispatchedRegion.IsNone()))
                input = DirtyRegion::Whole();
            pending.undispatchedRegion.Union(input);
        }

        if (queues)
        {
            // The bridge must exist before ProcessDeferredCompute drives it.
            GetOrCreateEffect(dc, node);
            m_computeQueue.entries.push_back({ node.id, std::move(inputImages) });
            m_computeQueue.queued.insert(node.id);
        }
        // Not dispatching: a consumer that now reads its values on the CPU
        // (a binding just added, a fallback build) reads the GPU results.
        else if (gpuOnlyResults && m_frameOptions.skipUnneededCpuReadback)
        {
            ReadBackIfCpuNeeded(graph, node, [this](uint32_t producerId)
            {
                return AnalysisBitmap(producerId) != nullptr || m_computeQueue.queued.contains(producerId);
            });
        }
        // A frozen pass cannot queue, so a change it finds stays pending for
        // the next frame rather than being dropped.
        if (!m_frozen)
            node.ClearDirty();
        if (!hasImageOutput)
            node.cachedOutput = nullptr;
    }

    std::unordered_set<uint32_t> EvaluatorCore::PlanReadbacks(const GraphModel& graph)
    {
        // A producer publishes its lane-3 texture only once a consumer asks,
        // and must be asked before the dispatch that fills it -- so every
        // binding the plan can serve through a texture requests one, whatever
        // the skip setting. The skip setting decides only whether the CPU Map
        // is skipped too: with it on, a node reads back when the host hinted
        // it (throttled) or when a binding reading it is not served on the
        // GPU this frame.
        const bool skipFlag = m_frameOptions.skipUnneededCpuReadback;
        std::unordered_set<uint32_t> needsReadback;
        if (skipFlag)
        {
            // Host hints are throttled; binding-driven readbacks are not,
            // because the consumer reads analysisOutput.fields every frame.
            const auto throttle = std::chrono::milliseconds(m_frameOptions.cpuAnalysisHintThrottleMs);
            const auto now = std::chrono::steady_clock::now();
            for (uint32_t id : m_cpuAnalysisInterest)
            {
                const auto* runtime = FindRuntime(id);
                const bool freshHint = !runtime || !runtime->lastHintReadback;
                const bool dueAgain = !freshHint && (now - *runtime->lastHintReadback) >= throttle;
                if (freshHint || dueAgain || throttle.count() == 0)
                    needsReadback.insert(id);
            }
        }
        // A pixel shader's build is chosen after the dispatches, by which
        // time every producer that has published or dispatches now has a
        // texture.
        auto publishedOrQueued = [this](uint32_t producerId)
        {
            return AnalysisBitmap(producerId) != nullptr || m_computeQueue.queued.contains(producerId);
        };
        for (const auto& consumerNode : graph.Nodes())
        {
            if (consumerNode.propertyBindings.empty()) continue;
            // What the consumer's lane can serve from the producers that exist.
            const GpuBindingPlan candidates = PlanGpuBindings(consumerNode, graph, m_frameOptions.gpuBindings,
                [this](uint32_t producerId) { return BridgeFor(producerId) != nullptr; });
            for (const auto& entry : candidates.entries)
            {
                if (entry.mode != Effects::GpuBindingMode::Texture) continue;
                m_computeQueue.textureWanted.insert(entry.producerId);
                winrt::com_ptr<Effects::IEngineComputeTexture> texture;
                if (SUCCEEDED(BridgeFor(entry.producerId)->QueryInterface(
                        __uuidof(Effects::IEngineComputeTexture), texture.put_void())))
                    texture->RequestAnalysisTexture();
            }
            // Whatever the skip says, so a readback a CPU-served binding
            // needs is counted as one with the skip off too (where only a
            // consumer this frame renders can need it).
            if (skipFlag || consumerNode.needed)
                CollectAnalysisDemand(consumerNode, graph, publishedOrQueued, m_computeQueue.cpuDemand,
                                  skipFlag ? &m_computeQueue.skipExpected : nullptr);
        }
        if (skipFlag)
            needsReadback.insert(m_computeQueue.cpuDemand.begin(), m_computeQueue.cpuDemand.end());
        return needsReadback;
    }

    void EvaluatorCore::CollectAnalysisDemand(const EffectNode& consumer, const GraphModel& graph,
        const std::function<bool(uint32_t)>& ready, std::unordered_set<uint32_t>& cpu,
        std::unordered_set<uint32_t>* gpu)
    {
        if (consumer.propertyBindings.empty()) return;
        // A pixel shader is judged by the build it will get, any other
        // consumer by the build it last ran: a compute consumer dispatches in
        // the same loop as its producers, and a fallback build reads the
        // cbuffer. Going by what could be served instead starves exactly
        // those frames: a silent wrong value, not a slow one.
        const bool pixelConsumer = GpuLaneFor(consumer) == Effects::GpuBindingMode::Texture;
        const GpuBindingPlan plan = pixelConsumer
            ? PlanGpuBindings(consumer, graph, m_frameOptions.gpuBindings, ready)
            : PlanGpuBindings(consumer, graph, m_frameOptions.gpuBindings,
                  [this](uint32_t producerId) { return BridgeFor(producerId) != nullptr; });
        const uint32_t expectedBits = pixelConsumer ? ExpectedTextureBits(consumer, graph, plan) : 0u;
        for (const auto& [propName, binding] : consumer.propertyBindings)
        {
            const auto* entry = plan.Find(propName);
            const bool served = entry && (pixelConsumer
                ? Effects::BindingModeAt(expectedBits, entry->gpuBindableIndex) == entry->mode
                : AppliedBindingMode(consumer.id, entry->gpuBindableIndex) == entry->mode);
            auto* into = served ? gpu : &cpu;
            if (into)
                binding.ForEachSourceNode([&](uint32_t sourceNodeId) { into->insert(sourceNodeId); });
        }
    }

    uint32_t EvaluatorCore::ExpectedTextureBits(const EffectNode& consumer, const GraphModel& graph,
                                                const GpuBindingPlan& plan)
    {
        // The choice ApplyCustomEffect makes: a build for the plan, if one is ready.
        if (plan.bits == 0 || consumer.type != NodeType::PixelShader) return 0;
        PropertyMap effectiveProps;
        ResolveBindings(consumer, graph, effectiveProps);
        const auto identity = Effects::CompileIdentityOf(consumer);
        const uint64_t optionKey = Effects::OptionKeyFor(consumer.customEffect.value(), effectiveProps);
        return m_variants.KeepsRouting(consumer, identity.effectId, identity.effectVersion, "ps_5_0",
                                       plan.bits, optionKey) ? plan.bits : 0u;
    }

    void EvaluatorCore::ReadBackIfCpuNeeded(GraphModel& graph, EffectNode& producer,
                                            const std::function<bool(uint32_t)>& ready)
    {
        NodeRuntime* runtime = FindRuntime(producer.id);
        if (!runtime || !runtime->cpuFieldsStale) return;
        // A binding read on the CPU first: that cause is the one counted.
        std::unordered_set<uint32_t> cpu;
        for (const auto& consumer : graph.Nodes())
        {
            if (cpu.contains(producer.id)) break;
            if (AnyBindingSource(consumer, [&](uint32_t id) { return id == producer.id; }))
                CollectAnalysisDemand(consumer, graph, ready, cpu, nullptr);
        }
        const bool forBinding = cpu.contains(producer.id);
        if (!forBinding && !m_cpuAnalysisInterest.contains(producer.id)) return;

        // The results the producer's last dispatch left are still current.
        std::vector<float> floats;
        auto* bridge = BridgeFor(producer.id);
        const HRESULT hr = bridge ? bridge->ReadResults(floats) : E_NOT_VALID_STATE;
        if (FAILED(hr))
        {
            producer.runtimeError = std::format(L"analysis readback failed at Map (0x{:08X}); fields are stale",
                static_cast<uint32_t>(hr));
            return;
        }
        ++(forBinding ? m_readbackStats.fallbackReadbacks : m_readbackStats.hintReadbacks);
        const auto previous = producer.analysisOutput.fields;
        UnpackAnalysisFloats(producer, floats);
        runtime->cpuFieldsStale = false;
        runtime->fieldsAreLastDispatch = true;
        PublishFieldsIfChanged(graph, producer, nullptr, true, &previous);
    }

    void EvaluatorCore::NoteFallbackBuild(const NodeVariantState& state)
    {
        ++(state.fallbackPending ? m_readbackStats.fallbackBuildsPending : m_readbackStats.fallbackBuilds);
    }

    ID2D1Bitmap1* EvaluatorCore::PreRenderInput(ID2D1DeviceContext5* dc, ID2D1Image* image,
        std::optional<std::pair<uint32_t, uint32_t>> producerPin, HRESULT* failHr)
    {
        ScopedDpi96 dpi(dc);

        // GetImageLocalBounds walks the image's effect chain back to its
        // source bitmap and access-violates inside d2d1.dll if an upstream
        // input is null. No C++ handler can catch that; the guard is the
        // source-readiness check at the top of ProcessDeferredCompute.
        D2D1_RECT_L px{};
        if (const HRESULT hr = GetSnappedPixelBounds(dc, image, px); FAILED(hr))
        {
            if (failHr) *failHr = hr;
            return nullptr;
        }
        const UINT32 w = static_cast<UINT32>(px.right - px.left);
        const UINT32 h = static_cast<UINT32>(px.bottom - px.top);

        // A bitmap the compute shader can read in place -- a source's image,
        // capture or video frame, an upstream compute node's image output, the
        // generator placeholder. The bridge binds its texture directly. Fresh,
        // because a producer in this pass dispatched earlier in topological order.
        if (ComputeReadableTexture(image, px))
        {
            winrt::com_ptr<ID2D1Bitmap1> asBitmap;
            image->QueryInterface(asBitmap.put());
            return asBitmap.get();
        }

        // Reuse the persistent target while its size still matches.
        PreRenderTarget& target = producerPin
            ? m_nodes[producerPin->first].preRenderTargets[producerPin->second]
            : m_unwiredPreRender;
        if (!target.bitmap || target.width != w || target.height != h)
        {
            D2D1_BITMAP_PROPERTIES1 bp{};
            bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            bp.dpiX = 96.0f;
            bp.dpiY = 96.0f;
            winrt::com_ptr<ID2D1Bitmap1> newBitmap;
            if (const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, newBitmap.put()); FAILED(hr))
            {
                ++m_preRenders.failures;
                if (failHr) *failHr = hr;
                return nullptr;
            }
            target = { std::move(newBitmap), w, h };
        }
        // Another image, or the same one placed elsewhere, changed everywhere.
        if (target.image != image || target.origin.x != px.left || target.origin.y != px.top)
            target.pending = DirtyRegion::Whole();

        // With dirty regions on, a producer's target redraws only where its
        // output changed since the last draw (EvaluatorCore::MarkOutputChanged
        // records it, whichever consumer runs); off, or for an unwired
        // placeholder, it redraws whole on every dispatch.
        const bool regions = m_frameOptions.dirtyRegions && producerPin.has_value();
        const DirtyRegion region = regions ? target.pending.ClippedTo(px) : DirtyRegion::Whole();
        m_preRenders.pixelsTarget += static_cast<uint64_t>(w) * h;
        if (region.IsNone())
        {
            ++m_preRenders.reused;
            return target.bitmap.get();
        }
        const bool whole = !region.IsRects() || region.Covers(px);

        // Its own draw session, whose EndDraw submits the draw before the
        // bridge's D3D11 compute reads it. Integer origin: see SnapComputeInputRect.
        HRESULT drawHr = S_OK;
        const bool injectFailure = std::exchange(m_failNextPreRenderDraw, false);
        {
            ScopedTarget scopedTarget(dc, target.bitmap.get());
            dc->BeginDraw();
            if (injectFailure)
                dc->Clear(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f));   // test hook: contents a retry must not trust
            else if (whole)
            {
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                dc->DrawImage(image, D2D1::Point2F(-static_cast<float>(px.left), -static_cast<float>(px.top)));
            }
            else
            {
                // Each rect clipped, no Clear: under SOURCE_COPY the image
                // replaces what the clip holds, transparent where it is.
                for (const auto& rect : region.Rects())
                {
                    const D2D1_RECT_F source{ static_cast<float>(rect.rect.left), static_cast<float>(rect.rect.top),
                                              static_cast<float>(rect.rect.right), static_cast<float>(rect.rect.bottom) };
                    const D2D1_RECT_F area{ source.left - px.left, source.top - px.top,
                                            source.right - px.left, source.bottom - px.top };
                    const D2D1_POINT_2F offset{ area.left, area.top };
                    dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
                    dc->DrawImage(image, &offset, &source, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                  D2D1_COMPOSITE_MODE_SOURCE_COPY);
                    dc->PopAxisAlignedClip();
                }
            }
            drawHr = dc->EndDraw();
            if (injectFailure && SUCCEEDED(drawHr)) drawHr = E_FAIL;
        }
        if (FAILED(drawHr))
        {
            // What the bitmap holds now is unknown.
            target.pending = DirtyRegion::Whole();
            ++m_preRenders.failures;
            if (failHr) *failHr = drawHr;
            return nullptr;
        }
        target.pending = {};
        target.image = image;
        target.origin = { px.left, px.top };
        ++m_preRenders.draws;
        if (whole)
        {
            ++m_preRenders.wholeDraws;
            m_preRenders.pixelsDrawn += static_cast<uint64_t>(w) * h;
        }
        else
        {
            m_preRenders.rectsDrawn += region.Rects().size();
            for (const auto& rect : region.Rects())
                m_preRenders.pixelsDrawn += static_cast<uint64_t>(rect.rect.right - rect.rect.left) *
                                            static_cast<uint64_t>(rect.rect.bottom - rect.rect.top);
        }
        return target.bitmap.get();
    }

    bool EvaluatorCore::ProcessDeferredCompute(GraphModel& graph, ID2D1DeviceContext5* dc)
    {
        m_policyCache.clear();
        // The last step of a frame: the deferred commit's clock moves on at
        // the next Normal Evaluate.
        struct CloseFrame
        {
            bool& open;
            ~CloseFrame() { open = false; }
        } closeFrame{ m_frameOpen };
        // Reset before the early returns: a frame that dispatches nothing reports 0.
        m_dispatchesLastFrame = 0;
        if (m_computeQueue.entries.empty() || !dc) return false;

        // A Source with no output yet (a video before its first frame, an
        // image still loading) leaves its chain unready: walking a downstream
        // effect's bounds through a null input access-violates inside
        // d2d1.dll. Skip the frame; the next one retries.
        for (const auto& n : graph.Nodes())
        {
            if (n.type == NodeType::Source && !n.cachedOutput)
            {
                // What the dropped dispatches would have consumed is unknown now.
                for (const auto& dropped : m_computeQueue.entries)
                    m_nodes[dropped.nodeId].undispatchedRegion = DirtyRegion::Whole();
                m_computeQueue.Clear();
                return false;
            }
        }

        bool imageComputeProduced = false;

        const bool skipFlag = m_frameOptions.skipUnneededCpuReadback;
        const std::unordered_set<uint32_t> needsReadback = PlanReadbacks(graph);

        // Each distinct upstream image is pre-rendered ONCE per pass (again
        // only where a refresh changed it) and shared across its consumers.
        // This is the only pre-render: it runs
        // inside the draw session and after any producer in the same pass has
        // dispatched, so it is fresh by construction.
        // The bitmap, and the target pixels its draw counted (0 for an image
        // read in place, which is not a target).
        std::unordered_map<ID2D1Image*, std::pair<ID2D1Bitmap1*, uint64_t>> sharedPreRender;
        auto preRenderShared = [&](ID2D1Image* image, std::optional<std::pair<uint32_t, uint32_t>> producerPin,
                                   HRESULT* failHr) -> ID2D1Bitmap1*
        {
            if (auto it = sharedPreRender.find(image); it != sharedPreRender.end())
            {
                // Unless a frozen refresh since changed the producer's output.
                bool current = true;
                if (m_frameOptions.dirtyRegions && producerPin)
                    if (const NodeRuntime* producer = FindRuntime(producerPin->first))
                        if (const auto target = producer->preRenderTargets.find(producerPin->second);
                            target != producer->preRenderTargets.end())
                            current = target->second.pending.IsNone();
                if (current)
                {
                    // Another use of the target, current: a reuse.
                    if (const uint64_t pixels = it->second.second; pixels > 0)
                    {
                        m_preRenders.pixelsTarget += pixels;
                        ++m_preRenders.reused;
                    }
                    return it->second.first;
                }
            }
            const uint64_t targetBefore = m_preRenders.pixelsTarget;
            ID2D1Bitmap1* bitmap = PreRenderInput(dc, image, producerPin, failHr);
            if (bitmap) sharedPreRender[image] = { bitmap, m_preRenders.pixelsTarget - targetBefore };
            return bitmap;
        };

        // Dispatch in topological order. The queue is filled across the
        // host's Evaluate passes, so a producer first queued in a later pass
        // can sit behind its consumer.
        {
            std::unordered_map<uint32_t, size_t> rank;
            try
            {
                const auto order = graph.TopologicalSort();
                for (size_t i = 0; i < order.size(); ++i)
                    rank[order[i]] = i;
            }
            catch (const std::logic_error&)
            {
                // A cycle: Evaluate queued nothing from it, keep the queue order.
            }
            std::stable_sort(m_computeQueue.entries.begin(), m_computeQueue.entries.end(),
                [&rank](const DeferredCompute& a, const DeferredCompute& b)
                { return rank[a.nodeId] < rank[b.nodeId]; });
        }

        // A dispatch changes its node's pixels and analysis values, but the
        // Direct2D nodes that read them were wired, sized and bound by
        // Evaluate before it ran. When a later consumer reads through one of
        // them, re-run that evaluation first, frozen so nothing is queued and
        // no readback opens a draw session.
        bool dispatchedSinceRefresh = false;
        // Dispatched without a readback on the strength of an expected GPU consumer.
        std::vector<uint32_t> expectedSkips;
        auto refreshUpstreamOf = [&](uint32_t consumerId)
        {
            if (!dispatchedSinceRefresh) return;

            // Walk the Direct2D nodes upstream of the consumer, stopping at
            // compute nodes: those are dispatched in order and need nothing.
            bool upstreamDirty = false;
            std::vector<uint32_t> queue = { consumerId };
            std::unordered_set<uint32_t> visited;
            for (size_t i = 0; i < queue.size() && !upstreamDirty; ++i)
            {
                for (const auto* edge : graph.GetInputEdges(queue[i]))
                {
                    const auto* source = graph.FindNode(edge->sourceNodeId);
                    if (!source || IsComputeNode(*source) || !visited.insert(source->id).second)
                        continue;
                    if (source->IsDirty()) { upstreamDirty = true; break; }
                    queue.push_back(source->id);
                }
            }
            if (!upstreamDirty) return;

            // Frozen, so this nested pass cannot append to the queue being iterated.
            Evaluate(graph, dc, EvalMode::Frozen);
            dispatchedSinceRefresh = false;
        };

        // A failed dispatch leaves no output rather than the previous one,
        // which would read as current.
        auto failDispatch = [&](EffectNode& failed, std::wstring message)
        {
            failed.runtimeError = std::move(message);
            // What the retry must consume is unknown now.
            auto& failedRuntime = m_nodes[failed.id];
            failedRuntime.undispatchedRegion = DirtyRegion::Whole();
            failedRuntime.fieldsAreLastDispatch = false;
            if (!failed.outputPins.empty() && failed.cachedOutput)
            {
                failed.cachedOutput = nullptr;
                graph.MarkDownstreamDirty(failed.id);
                imageComputeProduced = true;
            }
        };

        for (auto& deferred : m_computeQueue.entries)
        {
            auto* node = graph.FindNode(deferred.nodeId);
            if (!node || deferred.inputImages.empty()) continue;
            if (!node->customEffect.has_value()) continue;
            auto* bridge = BridgeFor(node->id);
            if (!bridge) continue;

            refreshUpstreamOf(node->id);

            // Resolve each input from the graph NOW rather than trusting the
            // image recorded at Evaluate time: a later pass in the same frame
            // can rebuild an upstream effect, and a producer dispatched earlier
            // in this loop has just replaced its output. The recorded image is
            // the fallback for a slot whose producer has no output.
            std::vector<ID2D1Bitmap1*> preRendered(deferred.inputImages.size(), nullptr);
            std::vector<ID2D1Image*>   inputRaw(deferred.inputImages.size(), nullptr);
            std::unordered_map<uint32_t, const EffectEdge*> edgeByPin;
            for (const auto* e : graph.GetInputEdges(deferred.nodeId))
                if (e) edgeByPin[e->destPin] = e;

            HRESULT preRenderHr = S_OK;
            size_t failedPin = 0;
            std::vector<bool> preRenderFailed(deferred.inputImages.size(), false);
            for (size_t i = 0; i < deferred.inputImages.size(); ++i)
            {
                std::optional<std::pair<uint32_t, uint32_t>> producerPin;
                ID2D1Image* img = deferred.inputImages[i].get();
                if (auto eit = edgeByPin.find(static_cast<uint32_t>(i)); eit != edgeByPin.end())
                {
                    producerPin = { eit->second->sourceNodeId, eit->second->sourcePin };
                    if (auto* src = graph.FindNode(eit->second->sourceNodeId); src && src->cachedOutput)
                        img = src->cachedOutput;
                }
                if (!img) continue;
                inputRaw[i] = img;
                HRESULT hr = S_OK;
                preRendered[i] = preRenderShared(img, producerPin, &hr);
                preRenderFailed[i] = FAILED(hr);
                if (FAILED(hr) && SUCCEEDED(preRenderHr)) { preRenderHr = hr; failedPin = i; }
            }
            if (preRenderHr == E_BOUNDS)
            {
                // An unbounded image (Flood, Tile, Border, Turbulence) cannot
                // be copied into a texture.
                failDispatch(*node, std::format(
                    L"Input {} has unbounded extent (Flood, Tile, Border or Turbulence?). "
                    L"Crop it before feeding a compute effect.", failedPin));
                continue;
            }
            if (preRenderHr == D2DERR_MAX_TEXTURE_SIZE_EXCEEDED)
            {
                failDispatch(*node, std::format(
                    L"Input {} is larger than {} px, the largest texture a compute effect can read. "
                    L"Scale or crop it first.", failedPin, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
                continue;
            }

            // A wired input without an image, or with an empty one, has an
            // upstream that is not ready (a shader still compiling, a failed
            // producer). Dispatching would mismatch the tiles a variadic node
            // counts with the sizes it derives.
            {
                std::optional<uint32_t> missingPin;
                for (const auto& [pin, edge] : edgeByPin)
                {
                    if (pin >= inputRaw.size()) continue;
                    if ((!inputRaw[pin] || preRenderFailed[pin]) && (!missingPin || pin < *missingPin))
                        missingPin = pin;
                }
                if (missingPin)
                {
                    failDispatch(*node, std::format(
                        L"Input {} has no image yet; its upstream is not ready", *missingPin));
                    continue;
                }
            }

            NodeRuntime& runtime = m_nodes[node->id];
            // A requested fresh read reads back whatever the skip says.
            const bool freshRequested = std::exchange(runtime.freshReadbackRequested, false);
            const bool readback = !skipFlag || needsReadback.contains(node->id) || freshRequested;
            // Deferred commit: a producer with a bound field that commits
            // after its dispatch reads back on every dispatch, without waiting
            // unless a cause above needs the values now.
            const FieldPolicies policies = DeferredCommitOn() ? CachedPoliciesOf(graph, *node) : FieldPolicies{};
            const bool deferredProducer = DeferredCommitOn() && policies.AnyDeferred();
            const bool deferredAsync = deferredProducer && !readback;
            // Async only with skip-readback on and the option set, and never
            // for a requested fresh read, which must block. A blocking read
            // retires the node's in-flight async copies, so none of them lands
            // later over the fresh values.
            const bool asyncReadback = deferredAsync ||
                (readback && skipFlag && !freshRequested && m_frameOptions.asyncAnalysisReadback);
            bridge->SetAsyncReadback(asyncReadback);
            // Whether the dispatch is over rects: what it will consume.
            DirtyRegion willConsume = runtime.undispatchedRegion;
            if (node->IsDirty())
                willConsume.Union(m_frameOptions.dirtyRegions ? node->Region() : DirtyRegion::Whole());
            const bool wholeInput = !willConsume.IsRects();
            // A cell reduction recomputes only the cells of input 0 the rects
            // touch, in the input texture's pixels: the image's rects less its
            // origin, which a pre-render or in-place binding puts at texel 0.
            // Only for a node whose sole wired image input is pin 0; a region
            // from another pin is in that pin's space. Set on every dispatch
            // (null: whole), so rects never outlive a dispatch that did not run.
            {
                std::vector<RECT> cellRects;
                bool cellRectsGiven = false;
                if (!wholeInput && m_frameOptions.incrementalReductions && !runtime.ownStateChanged &&
                    edgeByPin.size() == 1 && edgeByPin.contains(0) && inputRaw[0])
                {
                    ScopedDpi96 dpi(dc);
                    D2D1_RECT_L origin{};
                    if (SUCCEEDED(GetSnappedPixelBounds(dc, inputRaw[0], origin)))
                    {
                        cellRectsGiven = true;
                        for (const auto& rect : willConsume.Rects())
                            cellRects.push_back({ rect.rect.left - origin.left, rect.rect.top - origin.top,
                                                  rect.rect.right - origin.left, rect.rect.bottom - origin.top });
                    }
                }
                bridge->SetInputDirtyRects(cellRectsGiven ? cellRects.data() : nullptr,
                                           static_cast<UINT32>(cellRects.size()));
            }
            const bool filledBefore = bridge->AnalysisTextureFilled();
            const uint64_t generationBefore = bridge->BufferGeneration();
            const uint64_t seqBefore = bridge->LastScheduledReadback();
            if (deferredProducer)
            {
                PrepareDeferredDispatch(*node, runtime, bridge, policies, wholeInput);
            }
            else
            {
                bridge->SetTextureCopy(D3D11ComputeRunner::TextureCopy::All);
                // Off, or no longer deferred: the record describes nothing.
                if (runtime.deferred.dispatches > 0) runtime.deferred = {};
            }
            std::vector<float> landedFloats;
            bridge->SetComputeCommandList(m_frameOptions.computeCommandList);
            // What the bound consumers last rendered with, for the change gate.
            const bool previousKnown = m_frameOptions.analysisChangeGate && runtime.fieldsAreLastDispatch;
            std::vector<AnalysisFieldValue> previous;
            if (previousKnown) previous = node->analysisOutput.fields;
            // Exact per-node attribution: this dispatch is its own D3D11
            // submission. The non-flushing EndNode: a compute dispatch is not
            // D2D work, and a flush here would land in the middle of this
            // pass's target juggling inside the host's draw session.
            if (m_gpuTimer) m_gpuTimer->BeginNode(node->id);
            bool dispatched = false;
            if (std::exchange(m_failNextComputeDispatch, false))
                node->runtimeError = L"Injected dispatch failure (test hook)";
            else
                dispatched = DispatchViaBridge(dc, graph, *node, inputRaw, preRendered, bridge,
                                               readback || deferredAsync,
                                               deferredProducer ? &landedFloats : nullptr);
            if (m_gpuTimer) m_gpuTimer->EndNode(node->id);
            if (asyncReadback && bridge->HasPendingReadback())
                runtime.asyncReadbackPending = true;
            ++m_dispatchesLastFrame;

            // A hinted node's throttle window starts at its readback.
            // Binding-driven readbacks fire every frame and leave it alone.
            if (readback && skipFlag && m_cpuAnalysisInterest.contains(node->id))
                runtime.lastHintReadback = std::chrono::steady_clock::now();

            // Lane 3: the copy pass has just run, so the texture holds this
            // frame's values; wrapping it makes it attachable as an input. A
            // new view is a new input for its consumers.
            const ID2D1Bitmap1* viewBefore = runtime.analysisTexture.bitmap.get();
            if (m_computeQueue.textureWanted.contains(node->id))
                EnsureAnalysisBitmap(node->id, dc);
            const bool viewChanged = runtime.analysisTexture.bitmap.get() != viewBefore;

            // This dispatch consumed the node's pending change.
            if (dispatched && bridge->LastCellsTotal() > 0)
            {
                const bool incremental = bridge->LastCellsDispatched() < bridge->LastCellsTotal();
                ++m_reduceCells.dispatches;
                if (incremental) ++m_reduceCells.incremental;
                m_reduceCells.cellsDispatched += bridge->LastCellsDispatched();
                m_reduceCells.cellsTotal += bridge->LastCellsTotal();
                ++runtime.cellDispatches;
                if (incremental) ++runtime.cellIncremental;
                runtime.cellsDispatched += bridge->LastCellsDispatched();
                runtime.cellsTotal += bridge->LastCellsTotal();
                runtime.lastCellMask = bridge->LastCellMask();
                runtime.lastCellRectsGiven = bridge->LastCellRectsGiven();
            }
            if (dispatched)
            {
                // A frozen pass since the queue (refreshUpstreamOf) can have
                // re-dirtied the node; the dispatch read those inputs too.
                if (node->IsDirty())
                    runtime.undispatchedRegion.Union(m_frameOptions.dirtyRegions ? node->Region() : DirtyRegion::Whole());
                node->ClearDirty();
                dispatchedSinceRefresh = true;
                // An async readback lands later and fills the fields then.
                runtime.cpuFieldsStale = !readback && !deferredProducer;
                if (!readback && !deferredProducer && m_computeQueue.skipExpected.contains(node->id))
                    expectedSkips.push_back(node->id);

                if (deferredAsync)
                    ++m_readbackStats.deferredReadbacks;
                else if (!readback)
                    ++m_readbackStats.skippedReadbacks;
                else if (m_computeQueue.cpuDemand.contains(node->id))
                    ++m_readbackStats.fallbackReadbacks;
                else if (freshRequested)
                    ++m_readbackStats.freshReadbacks;
                else if (!skipFlag)
                    ++m_readbackStats.unskippedReadbacks;
                else
                    ++m_readbackStats.hintReadbacks;

                const DirtyRegion consumed = std::exchange(runtime.undispatchedRegion, {});
                runtime.lastDispatchInput = consumed.IsNone() ? DirtyRegion::Whole() : consumed;
                runtime.ownStateChanged = false;
                const bool current = readback && !asyncReadback && SUCCEEDED(bridge->LastReadbackResult()) &&
                                     !node->analysisOutput.fields.empty();
                runtime.fieldsAreLastDispatch = current;

                // Consumers BOUND to this node's analysis values, which no
                // edge reaches (a statistics node feeding a tone mapper's
                // SourcePeakNits has no image output), wake when a value they
                // read moved. The host's frozen post-dispatch Evaluate re-runs
                // them this frame, so a value is used the frame it is made.
                if (deferredProducer)
                {
                    // The record changes only when a readback lands, now for
                    // a blocking one, else on a later frame.
                    runtime.fieldsAreLastDispatch = false;
                    const bool blocking = readback && !asyncReadback && SUCCEEDED(bridge->LastReadbackResult());
                    FinishDeferredDispatch(graph, *node, runtime, bridge, policies, wholeInput, filledBefore,
                                           generationBefore, seqBefore, blocking ? &landedFloats : nullptr);
                    if (viewChanged) MarkBindingConsumersDirty(graph, node->id);
                }
                else
                {
                    const DirtyRegion judged = viewChanged ? DirtyRegion::Whole() : runtime.lastDispatchInput;
                    PublishFieldsIfChanged(graph, *node, &judged, current, previousKnown ? &previous : nullptr);
                }
            }
            else
            {
                runtime.undispatchedRegion = DirtyRegion::Whole();
                runtime.fieldsAreLastDispatch = false;
                MarkBindingConsumersDirty(graph, node->id);
                if (!node->compilePending)
                    failDispatch(*node, node->runtimeError);
            }

            if (dispatched && !node->outputPins.empty() && node->cachedOutput)
            {
                imageComputeProduced = true;
                graph.MarkDownstreamDirty(node->id);
            }
        }

        // Every producer has dispatched, so the textures that exist now are
        // the ones the frozen pass binds: a consumer expected on its GPU build
        // that will not get it is found, and read for, this frame.
        for (uint32_t producerId : expectedSkips)
            if (auto* producer = graph.FindNode(producerId))
                ReadBackIfCpuNeeded(graph, *producer,
                    [this](uint32_t id) { return AnalysisBitmap(id) != nullptr; });

        // A consumer removed from the graph must stop the producer paying for
        // lane 3. The producer's own RequestAnalysisTexture latch is sticky:
        // rebuilding the texture on alternate frames would cost more.
        m_computeQueue.Clear();
        return true;
    }

    D2D1_SIZE_U EvaluatorCore::ComputeImageOutputSize(
        const EffectNode& node, const GraphModel& graph,
        const std::vector<ID2D1Image*>& inputImages,
        const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
        ID2D1DeviceContext5* dc) const
    {
        const auto& def = node.customEffect.value();
        // An input's pixel size: its pre-render when there is one, else its bounds.
        auto inputSize = [&](uint32_t pin) -> D2D1_SIZE_U
        {
            if (pin < preRenderedInputs.size() && preRenderedInputs[pin])
                return preRenderedInputs[pin]->GetPixelSize();
            if (pin >= inputImages.size() || !inputImages[pin])
                return { 0, 0 };
            ScopedDpi96 dpi(dc);
            D2D1_RECT_L px{};
            if (FAILED(GetSnappedPixelBounds(dc, inputImages[pin], px)))
                return { 0, 0 };
            return D2D1::SizeU(px.right - px.left, px.bottom - px.top);
        };

        // An output size the effect derives from its inputs' sizes.
        const auto* deriveOutputSize = (def.shaderLabEffectId.empty() || !m_effectHooks)
            ? nullptr : m_effectHooks->ImageOutputSize(def.shaderLabEffectId);
        if (deriveOutputSize)
        {
            std::vector<D2D1_SIZE_U> inputSizes(inputImages.size(), D2D1_SIZE_U{ 0, 0 });
            for (const auto* edge : graph.GetInputEdges(node.id))
            {
                const uint32_t pin = edge->destPin;
                if (pin < inputImages.size() && inputImages[pin])
                    inputSizes[pin] = inputSize(pin);
            }
            const D2D1_SIZE_U size = (*deriveOutputSize)(node.properties, inputSizes);
            if (size.width != 0)
                return size;
        }

        if (const D2D1_SIZE_U size = ComputeOutputSizeFromProperties(def, node.properties); size.width != 0)
            return size;

        // Input 0's size, at least 64 x 64. Multi-input shaders still size
        // from t0: the contract is that all inputs match.
        D2D1_SIZE_U size = inputSize(0);
        size.width = (std::max)(size.width, 64u);
        size.height = (std::max)(size.height, 64u);
        return size;
    }

    bool EvaluatorCore::DispatchViaBridge(
        ID2D1DeviceContext5* dc,
        const GraphModel& graph,
        EffectNode& node,
        const std::vector<ID2D1Image*>& inputImages,
        const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
        Effects::CustomComputeBridgeEffect* bridge,
        bool readbackToCpu,
        std::vector<float>* readbackFloats)
    {
        if (!bridge) return false;
        auto& def = node.customEffect.value();
        NodeRuntime& runtime = m_nodes[node.id];

        // A baseline nobody compiled yet (the editor or /effect/compile
        // usually has) is compiled here; later dispatches reuse it.
        if (def.compiledBytecode.empty())
        {
            std::wstring error;
            switch (CompileBaseline(node, "cs_5_0", m_variants.AsyncCompile(), m_variants.CompilePrebuilt(), error))
            {
            case BaselineStatus::Pending:
                // Skip this dispatch and look again next frame.
                node.MarkDirty();
                return false;
            case BaselineStatus::Failed:
                node.runtimeError = error.empty() ? L"D3D11 compute shader compile failed" : std::move(error);
                return false;
            case BaselineStatus::Compiled:
                bridge->SetCompiledBytecode(def.compiledBytecode.data(), static_cast<UINT32>(def.compiledBytecode.size()));
                node.runtimeError.clear();
                break;
            }
        }

        // Lane 1: each served binding gets its producer's analysis SRV at
        // t<gpuBindableIndex + 1>; t0 is the input image.
        std::unordered_map<uint32_t, winrt::com_ptr<ID3D11ShaderResourceView>> producerSrvs;
        auto analysisSrv = [&](uint32_t producerId) -> ID3D11ShaderResourceView*
        {
            auto known = producerSrvs.find(producerId);
            if (known != producerSrvs.end()) return known->second.get();
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            winrt::com_ptr<Effects::IEngineComputeOutput> output;
            // The bridge impl, not the D2D effect: D2D's outer QueryInterface
            // does not reach the impl's interfaces.
            if (auto* producer = BridgeFor(producerId);
                producer && SUCCEEDED(producer->QueryInterface(__uuidof(Effects::IEngineComputeOutput), output.put_void())))
                output->GetAnalysisSrv(srv.put());
            return (producerSrvs[producerId] = std::move(srv)).get();
        };
        GpuBindingPlan bindingPlan = PlanGpuBindings(node, graph, m_frameOptions.gpuBindings,
            [&](uint32_t producerId) { return analysisSrv(producerId) != nullptr; });

        // The build for the GPU-routed bindings and specialised options. The
        // generic baseline is the last fallback and always correct.
        uint32_t macroBitset = bindingPlan.bits;
        const std::vector<uint8_t>* activeBytecode = &def.compiledBytecode;
        std::shared_ptr<const std::vector<uint8_t>> variantBytecode;
        m_variants.QueueOptionVariants(node, runtime.variant, "cs_5_0",
            PlanGpuBindings(node, graph, m_frameOptions.gpuBindings).bits);
        runtime.variant.fallbackInUse = false;
        runtime.variant.fallbackPending = false;
        if (const uint64_t optionKey = Effects::OptionKeyFor(def, node.properties); macroBitset != 0 || optionKey != 0)
        {
            const auto identity = Effects::CompileIdentityOf(node);
            const auto choice = m_variants.Select(node, runtime.variant, identity.effectId, identity.effectVersion,
                                                  "cs_5_0", macroBitset, optionKey);
            if (macroBitset != 0 && choice.bits == 0)
            {
                bindingPlan.entries.clear();
                runtime.variant.fallbackInUse = true;
                runtime.variant.fallbackPending = choice.routingPending;
                NoteFallbackBuild(runtime.variant);
            }
            macroBitset     = choice.bits;
            variantBytecode = choice.bytes;
            if (variantBytecode)
                activeBytecode = variantBytecode.get();
        }
        runtime.variant.appliedGpuBits = macroBitset;

        // Install whatever the plan implies before every dispatch. The bridge
        // compares by DXBC checksum, so this is a no-op unless the plan changed.
        if (!activeBytecode->empty())
            bridge->SetCompiledBytecode(activeBytecode->data(), static_cast<UINT32>(activeBytecode->size()));

        UINT32 analysisFloat4Count = 0;
        for (const auto& f : def.analysisFields)
            analysisFloat4Count += f.pixelCount();

        // A variadic node's connected pins: bit i set = pin i has an image.
        if (def.variadicInputs && HasInjected(def.injectedConstants, InjectedConstants::InputMask))
        {
            uint32_t inputMask = 0;
            for (const auto* edge : graph.GetInputEdges(node.id))
                if (edge->destPin < 32 && edge->destPin < inputImages.size() && inputImages[edge->destPin])
                    inputMask |= 1u << edge->destPin;
            node.properties[std::wstring(cInputMaskProperty)] = static_cast<float>(inputMask);
        }

        // 0 x 0 = analysis only (no u1 binding).
        const bool hasImageOutput = !node.outputPins.empty();
        const D2D1_SIZE_U imageOut = hasImageOutput
            ? ComputeImageOutputSize(node, graph, inputImages, preRenderedInputs, dc)
            : D2D1::SizeU(0, 0);
        if (imageOut.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || imageOut.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            node.runtimeError = std::format(
                L"Output would be {}x{} px, larger than {} px, the largest texture a compute effect can write. "
                L"Make it smaller or scale the inputs first.",
                imageOut.width, imageOut.height, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
            return false;
        }

        // The user part of the cbuffer, laid out by the build actually
        // installed (a GPU-served parameter has no value slot in a variant).
        const auto reflected = m_variants.ReflectCompute(*activeBytecode);
        std::vector<BYTE> cbBytes;
        if (!def.parameters.empty() && !activeBytecode->empty())
            cbBytes = PackComputeConstants(*reflected, node.properties, bindingPlan);

        // Pre-rendered bitmaps where there are any: D2D then does not
        // re-evaluate upstream effects with possibly different cached state.
        std::vector<ID2D1Image*> dispatchInputs(inputImages.size(), nullptr);
        for (size_t i = 0; i < inputImages.size(); ++i)
        {
            ID2D1Bitmap1* preRendered = i < preRenderedInputs.size() ? preRenderedInputs[i] : nullptr;
            dispatchInputs[i] = preRendered ? static_cast<ID2D1Image*>(preRendered) : inputImages[i];
        }

        // The bridge clears these at the end of Dispatch.
        for (const auto& entry : bindingPlan.entries)
            bridge->SetGpuBinding(entry.gpuBindableIndex + 1u, analysisSrv(entry.producerId));

        // A per-output-pixel shader is tiled over the output by its
        // [numthreads]. Every other shape keeps (1,1,1): one group that loops
        // itself, or a runner-sized reduction or image pass.
        if (hasImageOutput)
            if (const auto groups = PerPixelDispatchGroups(def, *reflected, imageOut.width, imageOut.height))
                bridge->SetDispatchDims((*groups)[0], (*groups)[1], (*groups)[2]);

        // A null output vector tells the bridge to skip the Map; the analysis
        // SRV is still filled for GPU consumers.
        std::vector<float> analysisFloats;
        const HRESULT hr = bridge->Dispatch(
            dc,
            dispatchInputs.data(),
            static_cast<UINT32>(dispatchInputs.size()),
            cbBytes.empty() ? nullptr : cbBytes.data(),
            static_cast<UINT32>(cbBytes.size()),
            analysisFloat4Count,
            imageOut.width, imageOut.height,
            readbackToCpu ? &analysisFloats : nullptr);

        if (!readbackToCpu)
            Performance::IncrementSkippedCpuReadbacks();

        if (FAILED(hr))
        {
            const auto stage = bridge->LastFailureStage();
            node.runtimeError = stage == Effects::CustomComputeBridgeEffect::DispatchStage::None
                ? std::format(L"Bridge dispatch failed 0x{:08X}", static_cast<uint32_t>(hr))
                : std::format(L"Bridge dispatch failed {} (0x{:08X})",
                      Effects::CustomComputeBridgeEffect::DescribeStage(stage), static_cast<uint32_t>(hr));
            return false;
        }
        node.runtimeError.clear();
        if (readbackToCpu && FAILED(bridge->LastReadbackResult()))
        {
            // The dispatch ran; only its values did not reach the CPU.
            node.runtimeError = std::format(L"analysis readback failed at Map (0x{:08X}); fields are stale",
                static_cast<uint32_t>(bridge->LastReadbackResult()));
        }

        // Only a readback that ran overwrites the fields: a skipped one, an
        // async one (PollAsyncReadbacks unpacks it when it lands) and a failed
        // Map all return nothing, and none may replace the values with zeros.
        // A deferred commit takes the floats to judge them itself.
        if (readbackFloats)
            *readbackFloats = std::move(analysisFloats);
        else if (readbackToCpu && analysisFloat4Count > 0 && !analysisFloats.empty())
            UnpackAnalysisFloats(node, analysisFloats);

        // Downstream nodes read the bridge's wrapped output texture.
        node.cachedOutput = hasImageOutput ? bridge->GetImageOutput() : nullptr;
        return true;
    }
}
