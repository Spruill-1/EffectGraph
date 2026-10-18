#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "InputPins.h"
#include "../D2DHelpers.h"
#include "../../Effects/ShaderVariants.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        constexpr std::wstring_view cGpuIndexPrefix = L"_SLIdx_";

        // Per-input extents a shader may declare, by destination pin.
        constexpr std::array<std::array<const wchar_t*, 2>, 4> cImageExtentNames{ {
            { L"ImageAW", L"ImageAH" },
            { L"ImageBW", L"ImageBH" },
            { L"ImageCW", L"ImageCH" },
            { L"ImageDW", L"ImageDH" },
        } };

        // Writes a float into both maps; true when the effective value moved.
        bool InjectFloat(EffectNode& node, PropertyMap& effectiveProps, const std::wstring& name, float value)
        {
            auto it = effectiveProps.find(name);
            const bool changed = it == effectiveProps.end() ||
                !std::holds_alternative<float>(it->second) ||
                std::get<float>(it->second) != value;
            effectiveProps[name] = value;
            node.properties[name] = value;
            return changed;
        }
    }

    std::optional<D2D1_SIZE_U> FixedPixelOutputSize(const CustomEffectDefinition& def, const PropertyMap& props)
    {
        const OutputExtent& extent = def.outputExtent;
        if (!extent.sizeParam.empty())
        {
            auto it = props.find(extent.sizeParam);
            if (it != props.end())
                if (const auto* size = std::get_if<float>(&it->second))
                    return D2D1::SizeU(static_cast<UINT32>(*size * extent.columns),
                                       static_cast<UINT32>(*size * extent.rows));
        }
        if (def.inputNames.empty())
            return D2D1::SizeU(512, 512);
        return std::nullopt;
    }

    bool EvaluatorCore::EnsurePixelBaseline(EffectNode& node)
    {
        auto& def = node.customEffect.value();
        if (def.isCompiled() || def.hlslSource.empty())
            return true;

        std::wstring error;
        switch (CompileBaseline(node, Effects::ShaderTargetFor(def), m_variants.AsyncCompile(), m_variants.CompilePrebuilt(), error))
        {
        case BaselineStatus::Pending:
            // No output yet; the node stays dirty so the next frame looks again.
            DropOutput(node);
            return false;
        case BaselineStatus::Failed:
            node.runtimeError = L"Auto-compile failed: " + error;
            DropOutput(node);
            // Retried on every visit regardless; see VisitBuiltIn.
            node.dirty = false;
            return false;
        case BaselineStatus::Compiled:
            CoCreateGuid(&def.shaderGuid);
            node.dirty = true;
            return true;
        }
        return true;
    }

    void EvaluatorCore::VisitCustomEffect(GraphModel& graph, EffectNode& node, ID2D1DeviceContext5* dc)
    {
        if (node.customEffect.has_value() && !EnsurePixelBaseline(node))
            return;

        std::wstring createError;
        ID2D1Effect* effect = GetOrCreateEffect(dc, node, &createError);
        if (!effect)
        {
            // A node with no CLSID at all failed at load; that error names the cause.
            const bool keepLoadError = !node.effectClsid.has_value() &&
                !node.customEffect.has_value() && !node.runtimeError.empty();
            if (!keepLoadError)
                node.runtimeError = std::wstring(cCreateEffectFailedPrefix) + createError;
            DropOutput(node);
            node.dirty = false;   // retried on every visit regardless
            return;
        }
        node.runtimeError.clear();
        NodeRuntime& runtime = m_nodes[node.id];

        if (!node.customEffect.has_value() || !node.customEffect->isCompiled())
        {
            if (node.customEffect.has_value())
            {
                node.runtimeError = L"Shader not compiled. Open in Effect Designer and compile.";
                DropOutput(node);
                node.dirty = false;
                return;
            }
            WireInputs(effect, node, graph);
            const bool wasDirty = node.dirty;
            if (wasDirty)
            {
                ApplyProperties(effect, node.properties);
                node.dirty = false;
            }
            UpdateEffectCachePolicy(effect, runtime, wasDirty);
            PublishOutput(node, effect);
            return;
        }

        const auto& def = node.customEffect.value();
        PropertyMap effectiveProps;
        const bool bindingsChanged = ResolveBindings(node, graph, effectiveProps);
        bool wasDirty = node.dirty || bindingsChanged;
        // A binding that moved changes this node's output, so downstream
        // consumers have to learn about it too.
        if (bindingsChanged) MarkOutputChanged(node.id);

        const bool isPixelShader = def.shaderType == CustomShaderType::PixelShader;

        // Tell a shader with an InputMask field which pins carry an image;
        // the rest hold the placeholder.
        if (isPixelShader && HasInjected(def.injectedConstants, InjectedConstants::InputMask))
        {
            uint32_t inputMask = 0;
            for (const auto* edge : graph.GetInputEdges(node.id))
            {
                const auto* sourceNode = graph.FindNode(edge->sourceNodeId);
                if (sourceNode && sourceNode->cachedOutput && edge->destPin < 32)
                    inputMask |= 1u << edge->destPin;
            }
            effectiveProps[std::wstring(cInputMaskProperty)] = static_cast<float>(inputMask);
            node.properties[std::wstring(cInputMaskProperty)] = static_cast<float>(inputMask);
        }
        if (isPixelShader && HasInjected(def.injectedConstants, InjectedConstants::OutputSize) &&
            InjectHostDimensions(node, effectiveProps, graph, dc))
            wasDirty = true;

        // Bound values go back to node.properties so the UI shows them live.
        if (bindingsChanged)
            WriteBackBindings(node, effectiveProps);

        if (wasDirty)
        {
            ApplyCustomEffect(effect, node, effectiveProps, graph, dc);

            // Upload the cbuffer now, with the shader its layout was reflected from.
            if (node.type == NodeType::PixelShader && runtime.instance.pixelImpl)
            {
                if (FAILED(runtime.instance.pixelImpl->ForceLoadShader()))
                    node.runtimeError = L"Loading the pixel shader variant failed";
                runtime.instance.pixelImpl->ForceUploadConstantBuffer();
            }
            else if (node.type == NodeType::ComputeShader && runtime.instance.computeImpl)
            {
                runtime.instance.computeImpl->ForceUploadConstantBuffer();
            }
            node.dirty = false;
        }

        WireInputs(effect, node, graph);

        // A generator draws its own content but still needs an input for sizing.
        if (def.inputNames.empty())
        {
            EnsureDummySourceBitmap(dc);
            if (m_dummySourceBitmap)
                effect->SetInput(0, m_dummySourceBitmap.get());
        }

        // Custom effects upload their cbuffer straight to the GPU, so D2D does
        // not know a re-render is needed. With caching on,
        // UpdateEffectCachePolicy drops this node's intermediate. With it off,
        // detach and reattach input 0 -- not used with caching on, because
        // detaching releases the upstream effect's cached intermediate too,
        // and one dirty node per frame then defeats caching for its whole
        // input chain.
        if (wasDirty && !m_frameOptions.effectOutputCaching)
        {
            winrt::com_ptr<ID2D1Image> savedInput;
            effect->GetInput(0, savedInput.put());
            if (savedInput)
            {
                effect->SetInput(0, nullptr);
                effect->SetInput(0, savedInput.get());
            }
        }

        UpdateEffectCachePolicy(effect, runtime, wasDirty);
        PublishOutput(node, effect);

        // Read analysis values back only when something changed (it costs a
        // draw session and a CPU map), and not on the frame the effect was
        // created.
        if (wasDirty &&
            def.analysisOutputType == AnalysisOutputType::Typed &&
            !def.analysisFields.empty() &&
            node.cachedOutput &&
            !m_pass.justCreated.contains(node.id))
            ReadCustomAnalysisOutput(dc, node);
    }

    bool EvaluatorCore::InjectHostDimensions(EffectNode& node, PropertyMap& effectiveProps,
                                             const GraphModel& graph, ID2D1DeviceContext5* dc)
    {
        // D2D pads input textures to atlas allocation sizes, so HLSL
        // GetDimensions() cannot give the true output rect; the shader reads
        // these instead. OutputW/H is the UNION of the input bounds, matching
        // CustomPixelShaderEffect::MapInputRectsToOutputRect, which produces
        // the rect the shader actually runs over. ImageAW/H.. give each
        // input's own content size, so the shader can tell content from
        // atlas padding.
        auto inputs = graph.GetInputEdges(node.id);
        if (inputs.empty() || !dc)
            return false;

        D2D1_RECT_F unionRect{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(),
                               -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
        bool anyValid = false;
        // Indexed by destination pin; GetInputEdges is in no particular order.
        std::vector<std::pair<float, float>> perInputWH(cImageExtentNames.size(), { 0.0f, 0.0f });
        {
            ScopedDpi96 dpi(dc);
            for (const auto& edge : inputs)
            {
                auto* srcNode = graph.FindNode(edge->sourceNodeId);
                if (!srcNode || !srcNode->cachedOutput) continue;
                D2D1_RECT_F b{};
                if (FAILED(dc->GetImageLocalBounds(srcNode->cachedOutput, &b)))
                    continue;
                const float bw = b.right - b.left;
                const float bh = b.bottom - b.top;
                if (bw <= 0 || bh <= 0) continue;
                unionRect.left   = (std::min)(unionRect.left,   b.left);
                unionRect.top    = (std::min)(unionRect.top,    b.top);
                unionRect.right  = (std::max)(unionRect.right,  b.right);
                unionRect.bottom = (std::max)(unionRect.bottom, b.bottom);
                anyValid = true;
                if (edge->destPin < perInputWH.size())
                    perInputWH[edge->destPin] = { bw, bh };
            }
        }
        if (!anyValid)
            return false;
        const float w = unionRect.right - unionRect.left;
        const float h = unionRect.bottom - unionRect.top;
        if (w <= 0 || h <= 0)
            return false;

        bool changed = InjectFloat(node, effectiveProps, std::wstring(cOutputWProperty), w);
        changed = InjectFloat(node, effectiveProps, std::wstring(cOutputHProperty), h) || changed;

        auto declaresParam = [&](const wchar_t* name)
        {
            for (const auto& p : node.customEffect->parameters)
                if (p.name == name) return true;
            return false;
        };
        for (size_t i = 0; i < cImageExtentNames.size(); ++i)
        {
            const wchar_t* nameW = cImageExtentNames[i][0];
            const wchar_t* nameH = cImageExtentNames[i][1];
            if (!declaresParam(nameW) || !declaresParam(nameH)) continue;
            // An input with no output yet (a compute branch just wired in) takes
            // the union size: "atlas == content", which is right for compute
            // outputs, the usual case.
            float iw = perInputWH[i].first;
            float ih = perInputWH[i].second;
            if (iw <= 0 || ih <= 0) { iw = w; ih = h; }
            const bool widthChanged = InjectFloat(node, effectiveProps, nameW, iw);
            const bool heightChanged = InjectFloat(node, effectiveProps, nameH, ih);
            if (widthChanged || heightChanged) changed = true;
        }
        return changed;
    }

    void EvaluatorCore::ApplyCustomEffect(ID2D1Effect* effect, EffectNode& node, const PropertyMap& effectiveProps,
                                          const GraphModel& graph, ID2D1DeviceContext5* dc)
    {
        if (!node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();
        auto* runtime = FindRuntime(node.id);
        if (!runtime || !runtime->instance.HasCustomImpl())
            return;
        auto& instance = runtime->instance;

        // ---- lane 3: analysis textures into a pixel shader ----------------
        // The plan decides which build to load, so it comes first. A
        // parameter goes texture-mode only once its producer has published a
        // bitmap; before the first compute dispatch it stays cbuffer-bound
        // and reads the CPU value -- correct, just slower.
        GpuBindingPlan analysisPlan = PlanGpuBindings(node, graph, m_frameOptions.gpuBindings,
            [this](uint32_t producerId) { return AnalysisBitmap(producerId) != nullptr; });
        uint32_t gpuModeBits = analysisPlan.bits;

        // The build the plan implies; the baseline on a miss keeps the node
        // rendering (cbuffer mode, CPU values). Specialised options apply to
        // pixel shaders only; a D2D-tiled compute node runs its generic build.
        const std::vector<uint8_t>* activeBytecode = &def.compiledBytecode;
        std::shared_ptr<const std::vector<uint8_t>> variantBytes;
        uint64_t optionKey = 0;
        if (node.type == NodeType::PixelShader)
        {
            m_variants.QueueOptionVariants(node, runtime->variant, "ps_5_0",
                PlanGpuBindings(node, graph, m_frameOptions.gpuBindings).bits);
            optionKey = Effects::OptionKeyFor(def, effectiveProps);
        }
        if (gpuModeBits != 0 || optionKey != 0)
        {
            const auto identity = Effects::CompileIdentityOf(node);
            const auto choice = m_variants.Select(node, runtime->variant, identity.effectId, identity.effectVersion,
                                                  "ps_5_0", gpuModeBits, optionKey);
            if (gpuModeBits != 0 && choice.bits == 0)
                analysisPlan.entries.clear();
            gpuModeBits  = choice.bits;
            optionKey    = choice.options;
            variantBytes = choice.bytes;
            if (variantBytes && !variantBytes->empty())
                activeBytecode = variantBytes.get();
        }
        runtime->variant.appliedGpuBits = gpuModeBits;

        // WireInputs (next) fills unwired lookup and variadic pins with the
        // placeholder, and has no device context to create it with.
        if (effect && node.type == NodeType::PixelShader && (def.lookupInputCount > 0 || def.variadicInputs))
            EnsureAnalysisDummy(dc);

        // Fill EVERY reserved pin -- bound ones with their analysis bitmap,
        // the rest with the placeholder: D2D renders nothing at all for a
        // custom effect with an unconnected input.
        if (effect && node.type == NodeType::PixelShader)
        {
            const uint32_t declared = DeclaredInputCount(def);
            const uint32_t total    = TotalInputCount(node, def);
            if (total > declared)
            {
                EnsureAnalysisDummy(dc);
                for (uint32_t pin = declared; pin < total; ++pin)
                {
                    ID2D1Image* desired = static_cast<ID2D1Image*>(m_analysisDummyBitmap.get());
                    for (const auto& entry : analysisPlan.entries)
                        if (declared + entry.gpuBindableIndex == pin)
                        { desired = static_cast<ID2D1Image*>(AnalysisBitmap(entry.producerId)); break; }
                    if (!desired) continue;
                    winrt::com_ptr<ID2D1Image> current;
                    effect->GetInput(pin, current.put());
                    // Only on change: re-setting an input D2D already holds
                    // counts as a topology change and drops the cached
                    // intermediate.
                    if (current.get() != desired)
                        effect->SetInput(pin, desired);
                }
            }
        }

        // Load the shader only when the wanted build differs from the loaded one.
        if (node.type == NodeType::PixelShader && instance.pixelImpl)
        {
            const BytecodeId wanted = IdOfBytecode(*activeBytecode);
            const auto& loaded = runtime->variant.loadedPixelVariant;
            if (instance.pixelImpl->NeedsShaderLoad() || !loaded || *loaded != wanted)
            {
                instance.pixelImpl->SetShaderGuid(GraphEvaluator::ShaderGuidFor(def.shaderGuid, *activeBytecode));
                instance.pixelImpl->LoadShaderBytecode(activeBytecode->data(),
                    static_cast<UINT32>(activeBytecode->size()));
                runtime->variant.loadedPixelVariant = wanted;
            }
        }
        else if (node.type == NodeType::ComputeShader && instance.computeImpl)
        {
            instance.computeImpl->SetShaderGuid(GraphEvaluator::ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            instance.computeImpl->LoadShaderBytecode(def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            instance.computeImpl->SetThreadGroupSize(def.threadGroupX, def.threadGroupY);
        }

        // Reflect what is actually loaded: a texture-mode parameter swaps its
        // cbuffer value slot for a uint index slot, so every later offset moves.
        const auto reflection = m_variants.ReflectPixel(*activeBytecode);
        if (reflection->constantBuffers.empty())
            return;
        const auto& cb = reflection->constantBuffers[0];
        std::vector<BYTE> cbData(cb.sizeBytes, 0);

        for (const auto& var : cb.variables)
        {
            // _TileOffset is written per tile in CalculateThreadgroups.
            if (node.type == NodeType::ComputeShader && var.name == L"_TileOffset")
                continue;

            auto propIt = effectiveProps.find(std::wstring(var.name.begin(), var.name.end()));
            if (propIt == effectiveProps.end()) continue;

            // Typed pack: a float-stored enum goes into a uint/int/bool slot
            // as that type, not as a float bit pattern.
            if (var.offset < cbData.size())
            {
                Effects::PackPropertyToCBuffer(cbData.data() + var.offset,
                    static_cast<uint32_t>(cbData.size()) - var.offset,
                    var.type, var.columns, propIt->second);
            }
        }

        // Lane 3: the variant replaced each bound parameter's value slot with
        // `uint _SLIdx_<name>`; the host supplies the field index it loads.
        for (const auto& entry : analysisPlan.entries)
        {
            const std::wstring slot = std::wstring(cGpuIndexPrefix) + entry.paramName;
            for (const auto& var : cb.variables)
            {
                if (var.name != slot) continue;
                if (var.offset + sizeof(uint32_t) > cbData.size()) break;
                const uint32_t v = entry.fieldIndex;
                std::memcpy(cbData.data() + var.offset, &v, sizeof(v));
                break;
            }
        }

        // Derived constants: parameter-only tables a built-in computes on the
        // CPU instead of per pixel. Written only into variables this bytecode
        // declares, so older saved HLSL is untouched.
        if (!def.shaderLabEffectId.empty() && m_effectHooks)
        {
            if (const auto* deriveConstants = m_effectHooks->DerivedConstants(def.shaderLabEffectId))
            {
                // A table whose size differs from the shader's variable means
                // the C++ and the HLSL disagree on its layout.
                std::wstring layoutError;
                (*deriveConstants)(effectiveProps,
                    [&](std::wstring_view name, const float* data, size_t floatCount)
                    {
                        for (const auto& var : cb.variables)
                        {
                            if (var.name != name) continue;
                            if (floatCount * sizeof(float) != var.size)
                                layoutError += std::format(L" {} is {} floats, the shader declares {};",
                                    name, floatCount, var.size / sizeof(float));
                            if (var.offset >= cbData.size()) return;
                            size_t bytes = (std::min)(floatCount * sizeof(float), static_cast<size_t>(var.size));
                            bytes = (std::min)(bytes, cbData.size() - var.offset);
                            std::memcpy(cbData.data() + var.offset, data, bytes);
                            return;
                        }
                    });
                if (!layoutError.empty())
                    node.runtimeError = L"Derived constants do not fit:" + layoutError;
            }
        }

        if (node.type == NodeType::PixelShader && instance.pixelImpl)
        {
            instance.pixelImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
            // A declared output extent fixes the size (a diagram viewer whose
            // shader loops over its input per output pixel would otherwise
            // render at the full source size); a generator always has one.
            if (const auto size = FixedPixelOutputSize(def, effectiveProps))
                instance.pixelImpl->SetFixedOutputSize(size->width, size->height);
        }
        else if (node.type == NodeType::ComputeShader && instance.computeImpl)
        {
            instance.computeImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
        }
    }
}
