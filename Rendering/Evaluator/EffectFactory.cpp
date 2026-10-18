#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "InputPins.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        // The CLSID a custom effect registers under. A CLSID registers once
        // with a fixed input count, so each input count gets its own.
        GUID CustomEffectClsid(const CustomEffectDefinition& def, uint32_t inputCount)
        {
            GUID clsid = def.shaderGuid;
            clsid.Data1 ^= inputCount;
            return clsid;
        }
    }

    ID2D1Effect* EvaluatorCore::GetOrCreateEffect(ID2D1DeviceContext5* dc, const EffectNode& node,
                                                  std::wstring* failureReason)
    {
        if (auto* runtime = FindRuntime(node.id); runtime && runtime->instance.effect)
            return runtime->instance.effect.get();

        std::wstring registrationError;
        GUID clsid{};
        const bool isD3D11Compute = node.customEffect.has_value() &&
            node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;
        const bool isCustomD2D = !isD3D11Compute &&
            (node.type == NodeType::PixelShader || node.type == NodeType::ComputeShader) &&
            node.customEffect.has_value() && node.customEffect->isCompiled();

        if (isD3D11Compute)
        {
            // D3D11 compute nodes share the bridge effect (registered once at
            // engine startup); its dispatch runs in ProcessDeferredCompute and
            // its bytecode is set per instance.
            clsid = Effects::CustomComputeBridgeEffect::CLSID_CustomComputeBridge;
        }
        else if (isCustomD2D)
        {
            const uint32_t inputCount = TotalInputCount(node, node.customEffect.value());
            clsid = CustomEffectClsid(node.customEffect.value(), inputCount);

            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            if (auto factory1 = factory.as<ID2D1Factory1>())
            {
                // Registering a CLSID again succeeds (D2D counts registrations).
                const HRESULT regHr = node.type == NodeType::PixelShader
                    ? Effects::CustomPixelShaderEffect::RegisterWithInputCount(factory1.get(), clsid, inputCount)
                    : Effects::CustomComputeShaderEffect::RegisterWithInputCount(factory1.get(), clsid, inputCount);
                if (FAILED(regHr))
                {
                    registrationError = std::format(L"registering the effect with {} inputs failed (0x{:08X})",
                        inputCount, static_cast<uint32_t>(regHr));
                    OutputDebugStringW(std::format(L"[CustomFX] node {} {}\n", node.id, registrationError).c_str());
                }
            }
        }
        else if (node.effectClsid.has_value())
        {
            clsid = node.effectClsid.value();
        }
        else
        {
            if (failureReason)
                *failureReason = node.customEffect.has_value() ? L"shader is not compiled" : L"node has no effect CLSID";
            return nullptr;
        }

        // Custom classes also hand back their impl. The input count must match
        // the registration exactly: D2D sizes the transform's input array from it.
        EffectInstance instance;
        HRESULT hr = E_FAIL;
        if (isD3D11Compute)
            hr = Effects::CustomComputeBridgeEffect::Create(dc, instance.effect, &instance.bridgeImpl);
        else if (node.type == NodeType::PixelShader && node.customEffect.has_value())
            hr = Effects::CustomPixelShaderEffect::Create(dc, clsid,
                TotalInputCount(node, node.customEffect.value()), instance.effect, &instance.pixelImpl);
        else if (node.type == NodeType::ComputeShader && node.customEffect.has_value())
            hr = Effects::CustomComputeShaderEffect::Create(dc, clsid,
                TotalInputCount(node, node.customEffect.value()), instance.effect, &instance.computeImpl);
        else
            hr = dc->CreateEffect(clsid, instance.effect.put());

        if (FAILED(hr))
        {
            wchar_t guidStr[64]{};
            StringFromGUID2(clsid, guidStr, 64);
            OutputDebugStringW(std::format(L"[CustomFX] CreateEffect({}) FAILED hr=0x{:08X}\n",
                guidStr, static_cast<uint32_t>(hr)).c_str());
            if (failureReason)
            {
                *failureReason = registrationError.empty()
                    ? std::format(L"CreateEffect failed (0x{:08X})", static_cast<uint32_t>(hr))
                    : registrationError;
            }
            return nullptr;
        }

        if (auto* pixelImpl = instance.pixelImpl)
        {
            // The first GeometricInputCount pins decide the output bounds; the
            // rest are lookup tables and reserved analysis pins.
            pixelImpl->SetImageInputCount(GeometricInputCount(node.customEffect.value()));
            pixelImpl->SetSubRectInputDemand(m_frameOptions.subRectInputDemand);
            // A generator needs its size before its first apply.
            if (node.customEffect->inputNames.empty())
            {
                if (const auto size = FixedPixelOutputSize(node.customEffect.value(), node.properties))
                    pixelImpl->SetFixedOutputSize(size->width, size->height);
            }
        }
        else if (auto* bridge = instance.bridgeImpl)
        {
            // Install bytecode the host already compiled (/effect/compile, the
            // editor); otherwise the first dispatch compiles it.
            if (node.customEffect.has_value() && !node.customEffect->compiledBytecode.empty())
            {
                bridge->SetCompiledBytecode(node.customEffect->compiledBytecode.data(),
                    static_cast<UINT32>(node.customEffect->compiledBytecode.size()));
            }
        }

        NodeRuntime& runtime = m_nodes[node.id];
        // A new instance starts with D2D's default (uncached), whatever the old one had.
        runtime.instance = std::move(instance);
        m_pass.justCreated.insert(node.id);
        return runtime.instance.effect.get();
    }

    void EvaluatorCore::ApplyProperties(ID2D1Effect* effect, const PropertyMap& effectiveProps)
    {
        if (!effect)
            return;

        // Built-in D2D effects use indexed properties. An all-digit key is the
        // index; any other key is looked up in the effect's name table. An
        // all-digit key too large for an index falls through to the name lookup.
        for (const auto& [key, value] : effectiveProps)
        {
            uint32_t index = UINT32_MAX;
            if (!key.empty() && std::all_of(key.begin(), key.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
            {
                uint64_t parsed = 0;
                for (wchar_t ch : key)
                {
                    parsed = parsed * 10 + static_cast<uint64_t>(ch - L'0');
                    if (parsed >= UINT32_MAX) break;
                }
                if (parsed < UINT32_MAX)
                    index = static_cast<uint32_t>(parsed);
            }
            if (index == UINT32_MAX)
            {
                index = effect->GetPropertyIndex(key.c_str());
                if (index == UINT32_MAX)
                    continue;
            }

            std::visit([effect, index](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                namespace Numerics = winrt::Windows::Foundation::Numerics;
                if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    effect->SetValue(index, static_cast<BOOL>(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    // String properties are rare in D2D effects; skip.
                }
                else if constexpr (std::is_same_v<T, Numerics::float2>)
                {
                    effect->SetValue(index, D2D1_VECTOR_2F{ v.x, v.y });
                }
                else if constexpr (std::is_same_v<T, Numerics::float3>)
                {
                    effect->SetValue(index, D2D1_VECTOR_3F{ v.x, v.y, v.z });
                }
                else if constexpr (std::is_same_v<T, Numerics::float4>)
                {
                    effect->SetValue(index, D2D1_VECTOR_4F{ v.x, v.y, v.z, v.w });
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    effect->SetValue(index, D2D1_PROPERTY_TYPE_MATRIX_5X4,
                        reinterpret_cast<const BYTE*>(&v), sizeof(v));
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    effect->SetValue(index, reinterpret_cast<const BYTE*>(v.data()),
                        static_cast<UINT32>(v.size() * sizeof(float)));
                }
            }, value);
        }
    }

    void EvaluatorCore::WireInputs(ID2D1Effect* effect, const EffectNode& destNode, const GraphModel& graph)
    {
        if (!effect)
            return;

        const UINT32 totalInputs = effect->GetInputCount();
        std::vector<bool> connected(totalInputs, false);

        // Re-setting an input D2D already holds may count as a topology change
        // and drop the effect's D2D1_PROPERTY_CACHED intermediate.
        auto setInputIfChanged = [effect](UINT32 pin, ID2D1Image* desired)
        {
            winrt::com_ptr<ID2D1Image> current;
            effect->GetInput(pin, current.put());
            if (current.get() != desired)
                effect->SetInput(pin, desired);
        };

        for (const auto* edge : graph.GetInputEdges(destNode.id))
        {
            if (edge->destPin >= totalInputs)
                continue;
            const EffectNode* srcNode = graph.FindNode(edge->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
            {
                setInputIfChanged(edge->destPin, srcNode->cachedOutput);
                connected[edge->destPin] = true;
            }
        }

        // Clear pins no longer connected -- but only the declared ones. The
        // pins past them carry lane-3 textures, attached by ApplyCustomEffect
        // rather than by an edge; clearing them would leave the shader
        // sampling an empty pin it is compiled to read from.
        UINT32 clearLimit = totalInputs;
        UINT32 firstLookup = totalInputs;
        if (destNode.type == NodeType::PixelShader && destNode.customEffect.has_value())
        {
            clearLimit = (std::min)(totalInputs, DeclaredInputCount(destNode.customEffect.value()));
            firstLookup = (std::min)(clearLimit, GeometricInputCount(destNode.customEffect.value()));
        }
        const bool variadic = destNode.customEffect.has_value() && destNode.customEffect->variadicInputs;
        for (UINT32 i = 0; i < clearLimit; ++i)
        {
            if (connected[i]) continue;
            // An unwired lookup pin, and any unconnected pin of a variadic
            // node, gets the placeholder rather than null: D2D renders nothing
            // for an effect with a null input. Its alpha of 0 is how the shader
            // reads "no table".
            ID2D1Image* fill = nullptr;
            if ((i >= firstLookup || variadic) && m_analysisDummyBitmap)
                fill = static_cast<ID2D1Image*>(m_analysisDummyBitmap.get());
            setInputIfChanged(i, fill);
        }
    }

    void EvaluatorCore::EnsureAnalysisDummy(ID2D1DeviceContext5* dc)
    {
        if (m_analysisDummyBitmap || !dc) return;
        // 1x1 and zero-filled: what an unbound cbuffer slot would read.
        const float zero[4]{ 0, 0, 0, 0 };
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_IGNORE));
        const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(1, 1), zero, sizeof(zero), &props,
                                            m_analysisDummyBitmap.put());
        if (FAILED(hr))
            std::fwprintf(stderr, L"[ShaderLab] analysis placeholder bitmap creation failed hr=0x%08X\n",
                static_cast<unsigned>(hr));
    }

    void EvaluatorCore::EnsureDummySourceBitmap(ID2D1DeviceContext5* dc)
    {
        if (m_dummySourceBitmap) return;
        // The output rect comes from SetFixedOutputSize, but D2D needs the
        // input to be at least as large as a generator's default output.
        D2D1_BITMAP_PROPERTIES1 props = {};
        props.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(2048, 2048), nullptr, 0, props, m_dummySourceBitmap.put());
        if (FAILED(hr))
            std::fwprintf(stderr, L"[ShaderLab] source placeholder bitmap creation failed hr=0x%08X\n",
                static_cast<unsigned>(hr));
    }
}
