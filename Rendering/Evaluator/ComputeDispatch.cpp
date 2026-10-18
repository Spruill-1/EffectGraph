#include "pch_engine.h"
#include "ComputeDispatch.h"
#include "../../Effects/ShaderCompiler.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        // Bytes of Width/Height the runner writes at the start of the cbuffer.
        constexpr UINT cRunnerConstantBytes = 8;

        constexpr std::wstring_view cGpuIndexPrefix = L"_SLIdx_";
    }

    ReflectedComputeCb ReflectComputeConstants(const std::vector<uint8_t>& bytecode)
    {
        ReflectedComputeCb out;
        winrt::com_ptr<ID3D11ShaderReflection> reflect;
        if (FAILED(D3DReflect(bytecode.data(), bytecode.size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()))) || !reflect)
            return out;

        reflect->GetThreadGroupSize(&out.groupX, &out.groupY, &out.groupZ);
        // The parameters live in the cbuffer at b0; an image-pass shader also
        // has one at b1, which may come first in reflection order.
        ID3D11ShaderReflectionConstantBuffer* cbReflect = reflect->GetConstantBufferByIndex(0);
        D3D11_SHADER_DESC shaderDesc{};
        if (SUCCEEDED(reflect->GetDesc(&shaderDesc)))
        {
            for (UINT resourceIndex = 0; resourceIndex < shaderDesc.BoundResources; ++resourceIndex)
            {
                D3D11_SHADER_INPUT_BIND_DESC bind{};
                if (SUCCEEDED(reflect->GetResourceBindingDesc(resourceIndex, &bind)) &&
                    bind.Type == D3D_SIT_CBUFFER && bind.BindPoint == 0)
                {
                    cbReflect = reflect->GetConstantBufferByName(bind.Name);
                    break;
                }
            }
        }
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (!cbReflect || FAILED(cbReflect->GetDesc(&cbDesc)))
            return out;

        out.sizeBytes = cbDesc.Size;
        for (UINT v = 0; v < cbDesc.Variables; ++v)
        {
            auto* var = cbReflect->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC varDesc{};
            if (!var || FAILED(var->GetDesc(&varDesc))) continue;
            ReflectedCbVar rv;
            rv.name.assign(varDesc.Name, varDesc.Name + strlen(varDesc.Name));
            rv.offset = varDesc.StartOffset;
            D3D11_SHADER_TYPE_DESC typeDesc{};
            if (auto* typeInfo = var->GetType(); typeInfo && SUCCEEDED(typeInfo->GetDesc(&typeDesc)))
            {
                rv.type = typeDesc.Type;
                rv.cols = typeDesc.Columns;
            }
            out.vars.push_back(std::move(rv));
        }
        return out;
    }

    std::vector<BYTE> PackComputeConstants(
        const ReflectedComputeCb& cb,
        const std::map<std::wstring, PropertyValue>& properties,
        const GpuBindingPlan& plan)
    {
        std::vector<BYTE> bytes;
        if (cb.sizeBytes <= cRunnerConstantBytes)
            return bytes;
        bytes.assign(cb.sizeBytes - cRunnerConstantBytes, BYTE{ 0 });
        for (const auto& var : cb.vars)
        {
            if (var.offset < cRunnerConstantBytes) continue;
            const UINT32 destOff = var.offset - cRunnerConstantBytes;
            if (destOff >= bytes.size()) continue;
            const UINT32 remaining = static_cast<UINT32>(bytes.size() - destOff);

            // A GPU-served parameter's variant declares `uint _SLIdx_<name>`
            // in place of its value: the float4 index its shader loads.
            if (var.name.starts_with(cGpuIndexPrefix))
            {
                if (const auto* entry = plan.Find(std::wstring_view(var.name).substr(cGpuIndexPrefix.size()));
                    entry && remaining >= sizeof(uint32_t))
                    std::memcpy(bytes.data() + destOff, &entry->fieldIndex, sizeof(uint32_t));
                continue;
            }

            auto propIt = properties.find(var.name);
            if (propIt == properties.end()) continue;
            Effects::PackPropertyToCBuffer(bytes.data() + destOff, remaining, var.type, var.cols, propIt->second);
        }
        return bytes;
    }

    D2D1_SIZE_U ComputeOutputSizeFromProperties(
        const CustomEffectDefinition& def,
        const std::map<std::wstring, PropertyValue>& properties)
    {
        auto explicitExtent = [&](const wchar_t* name) -> UINT32
        {
            auto it = properties.find(name);
            if (it == properties.end()) return 0;
            if (const auto* f = std::get_if<float>(&it->second))
                return *f >= 1.0f ? static_cast<UINT32>(*f) : 0;
            if (const auto* u = std::get_if<uint32_t>(&it->second))
                return *u;
            return 0;
        };
        const UINT32 explicitW = explicitExtent(L"OutputWidth");
        const UINT32 explicitH = explicitExtent(L"OutputHeight");
        if (explicitW > 0 && explicitH > 0)
            return D2D1::SizeU(explicitW, explicitH);

        if (!def.outputExtent.sizeParam.empty())
        {
            auto it = properties.find(def.outputExtent.sizeParam);
            if (it != properties.end())
            {
                if (const auto* f = std::get_if<float>(&it->second))
                {
                    const UINT32 side = static_cast<UINT32>((std::max)(*f, 64.0f));
                    return D2D1::SizeU(side, side);
                }
                if (const auto* u = std::get_if<uint32_t>(&it->second))
                {
                    const UINT32 side = (std::max)(*u, 64u);
                    return D2D1::SizeU(side, side);
                }
            }
        }
        return D2D1::SizeU(0, 0);
    }

    std::optional<std::array<UINT32, 3>> PerPixelDispatchGroups(
        const CustomEffectDefinition& def, const ReflectedComputeCb& reflected,
        UINT32 width, UINT32 height)
    {
        if (width == 0 || height == 0 || !TilesPerOutputPixel(def.dispatchShape))
            return std::nullopt;
        UINT32 groupX = def.threadGroupX, groupY = def.threadGroupY;
        if (reflected.groupX > 0)
        {
            groupX = reflected.groupX;
            groupY = reflected.groupY;
        }
        if (groupX == 0 || groupY == 0)
            return std::nullopt;
        return std::array<UINT32, 3>{ (width + groupX - 1) / groupX, (height + groupY - 1) / groupY, 1 };
    }
}
