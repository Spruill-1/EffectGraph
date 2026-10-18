#include "pch_engine.h"
#include "ShaderIncludes.h"
#include "ShaderLabParamsHlsl.h"
#include "ShaderLabEffects.h"

#include <array>

namespace ShaderLab::Effects
{
    namespace
    {
        struct EmbeddedIncludeHandler : ID3DInclude
        {
            HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes) override
            {
                if (!pFileName || !ppData || !pBytes) return E_POINTER;
                const auto text = FindEmbeddedInclude(pFileName);
                if (!text) return E_FAIL;
                *ppData = text->data();
                *pBytes = static_cast<UINT>(text->size());
                return S_OK;
            }

            HRESULT __stdcall Close(LPCVOID) override
            {
                // The texts are static; nothing to free.
                return S_OK;
            }
        };

        EmbeddedIncludeHandler sIncludeHandler;
    }

    std::span<const EmbeddedInclude> EmbeddedIncludes()
    {
        static const std::array<EmbeddedInclude, 3> sIncludes{ {
            { kShaderLabParamsIncludeName, std::string_view(GetShaderLabParamsHLSL(), GetShaderLabParamsHLSLLength()) },
            { cColorMathIncludeName, GetColorMathHLSL() },
            { cGamutIncludeName, GetGamutHLSL() },
        } };
        return sIncludes;
    }

    std::optional<std::string_view> FindEmbeddedInclude(std::string_view name)
    {
        for (const auto& include : EmbeddedIncludes())
            if (include.name == name) return include.text;
        return std::nullopt;
    }

    ID3DInclude* ShaderLabIncludeHandler()
    {
        return &sIncludeHandler;
    }
}
