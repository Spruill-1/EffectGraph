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
                node.dirty = true;
                MarkOutputChanged(node.id);
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
        const bool needsCompute = node.dirty ||
            bindingSourceQueued ||
            (hasImageOutput && !node.cachedOutput) ||
            (!hasImageOutput && node.analysisOutput.fields.empty());
        // A pin fed by a compute node queued this frame has no image yet;
        // ProcessDeferredCompute resolves it after that producer dispatches.
        const bool inputFromQueuedProducer = std::any_of(inputs.begin(), inputs.end(),
            [this](const auto* edge) { return m_computeQueue.queued.contains(edge->sourceNodeId); });
        // Queued at most once per frame: the dispatch reads the node's state
        // when it runs, so one entry is always current.
        if ((primaryInput || inputFromQueuedProducer) && needsCompute && !m_frozen &&
            !m_computeQueue.queued.contains(node.id))
        {
            // The bridge must exist before ProcessDeferredCompute drives it.
            GetOrCreateEffect(dc, node);
            m_computeQueue.entries.push_back({ node.id, std::move(inputImages) });
            m_computeQueue.queued.insert(node.id);
        }
        // A frozen pass cannot queue, so a change it finds stays pending for
        // the next frame rather than being dropped.
        if (!m_frozen)
            node.dirty = false;
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
            if (!skipFlag) continue;

            // Skip a readback only for a binding the consumer's last build
            // actually read on the GPU. Texture mode engages once the producer
            // has published a bitmap, and a consumer on a fallback build reads
            // the cbuffer; both still need this frame's CPU value. Going by
            // what could be served instead starves exactly those frames: a
            // silent wrong value, not a slow one.
            for (const auto& [propName, binding] : consumerNode.propertyBindings)
            {
                const auto* entry = candidates.Find(propName);
                const bool served = entry &&
                    AppliedBindingMode(consumerNode.id, entry->gpuBindableIndex) == entry->mode;
                if (served) continue;
                binding.ForEachSourceNode([&](uint32_t sourceNodeId) { needsReadback.insert(sourceNodeId); });
            }
        }
        return needsReadback;
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
                if (failHr) *failHr = hr;
                return nullptr;
            }
            target = { std::move(newBitmap), w, h };
        }

        // Its own draw session, whose EndDraw submits the draw before the
        // bridge's D3D11 compute reads it. Integer origin: see SnapComputeInputRect.
        HRESULT drawHr = S_OK;
        {
            ScopedTarget scopedTarget(dc, target.bitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawImage(image, D2D1::Point2F(-static_cast<float>(px.left), -static_cast<float>(px.top)));
            drawHr = dc->EndDraw();
        }
        if (FAILED(drawHr))
        {
            if (failHr) *failHr = drawHr;
            return nullptr;
        }
        return target.bitmap.get();
    }

    bool EvaluatorCore::ProcessDeferredCompute(GraphModel& graph, ID2D1DeviceContext5* dc)
    {
        if (m_computeQueue.entries.empty() || !dc) return false;

        // A Source with no output yet (a video before its first frame, an
        // image still loading) leaves its chain unready: walking a downstream
        // effect's bounds through a null input access-violates inside
        // d2d1.dll. Skip the frame; the next one retries.
        for (const auto& n : graph.Nodes())
        {
            if (n.type == NodeType::Source && !n.cachedOutput)
            {
                m_computeQueue.Clear();
                return false;
            }
        }

        bool imageComputeProduced = false;
        m_dispatchesLastFrame = 0;

        const bool skipFlag = m_frameOptions.skipUnneededCpuReadback;
        const std::unordered_set<uint32_t> needsReadback = PlanReadbacks(graph);

        // Each distinct upstream image is pre-rendered ONCE per pass and
        // shared across its consumers. This is the only pre-render: it runs
        // inside the draw session and after any producer in the same pass has
        // dispatched, so it is fresh by construction.
        std::unordered_map<ID2D1Image*, ID2D1Bitmap1*> sharedPreRender;
        auto preRenderShared = [&](ID2D1Image* image, std::optional<std::pair<uint32_t, uint32_t>> producerPin,
                                   HRESULT* failHr) -> ID2D1Bitmap1*
        {
            if (auto it = sharedPreRender.find(image); it != sharedPreRender.end()) return it->second;
            ID2D1Bitmap1* bitmap = PreRenderInput(dc, image, producerPin, failHr);
            if (bitmap) sharedPreRender[image] = bitmap;
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
                    if (source->dirty) { upstreamDirty = true; break; }
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
            // Async only with skip-readback on and the option set, and never
            // for a requested fresh read, which must block. A blocking read
            // retires the node's in-flight async copies, so none of them lands
            // later over the fresh values.
            const bool asyncReadback = readback && skipFlag && !freshRequested &&
                m_frameOptions.asyncAnalysisReadback;
            bridge->SetAsyncReadback(asyncReadback);
            bridge->SetComputeCommandList(m_frameOptions.computeCommandList);
            // Exact per-node attribution: this dispatch is its own D3D11
            // submission. The non-flushing EndNode: a compute dispatch is not
            // D2D work, and a flush here would land in the middle of this
            // pass's target juggling inside the host's draw session.
            if (m_gpuTimer) m_gpuTimer->BeginNode(node->id);
            const bool dispatched = DispatchViaBridge(dc, graph, *node, inputRaw, preRendered, bridge, readback);
            if (m_gpuTimer) m_gpuTimer->EndNode(node->id);
            if (asyncReadback && bridge->HasPendingReadback())
                runtime.asyncReadbackPending = true;
            ++m_dispatchesLastFrame;

            // A hinted node's throttle window starts at its readback.
            // Binding-driven readbacks fire every frame and leave it alone.
            if (readback && skipFlag && m_cpuAnalysisInterest.contains(node->id))
                runtime.lastHintReadback = std::chrono::steady_clock::now();

            // Lane 3: the copy pass has just run, so the texture holds this
            // frame's values; wrapping it makes it attachable as an input.
            if (m_computeQueue.textureWanted.contains(node->id))
                EnsureAnalysisBitmap(node->id, dc);

            // Consumers BOUND to this node's analysis values are stale now too,
            // and no edge reaches them (a statistics node feeding a tone
            // mapper's SourcePeakNits has no image output). The host's frozen
            // post-dispatch Evaluate re-runs them this frame, so a value is
            // used the frame it is made.
            MarkBindingConsumersDirty(graph, node->id);

            // This dispatch consumed the node's pending change.
            if (dispatched)
            {
                node->dirty = false;
                dispatchedSinceRefresh = true;
            }
            else if (!node->compilePending)
            {
                failDispatch(*node, node->runtimeError);
            }

            if (dispatched && !node->outputPins.empty() && node->cachedOutput)
            {
                imageComputeProduced = true;
                graph.MarkDownstreamDirty(node->id);
            }
        }

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
        bool readbackToCpu)
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
                node.dirty = true;
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
        if (const uint64_t optionKey = Effects::OptionKeyFor(def, node.properties); macroBitset != 0 || optionKey != 0)
        {
            const auto identity = Effects::CompileIdentityOf(node);
            const auto choice = m_variants.Select(node, runtime.variant, identity.effectId, identity.effectVersion,
                                                  "cs_5_0", macroBitset, optionKey);
            if (macroBitset != 0 && choice.bits == 0)
                bindingPlan.entries.clear();
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
        if (readbackToCpu && analysisFloat4Count > 0 && !analysisFloats.empty())
            UnpackAnalysisFloats(node, analysisFloats);

        // Downstream nodes read the bridge's wrapped output texture.
        node.cachedOutput = hasImageOutput ? bridge->GetImageOutput() : nullptr;
        return true;
    }
}
