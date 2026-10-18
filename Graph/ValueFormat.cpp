#include "pch_engine.h"
#include "ValueFormat.h"

namespace ShaderLab::Graph
{
    namespace
    {
        std::wstring Fixed(float value, int decimals)
        {
            return std::vformat(L"{:.{}f}", std::make_wformat_args(value, decimals));
        }

        void AppendComponents(std::wstring& out, const float* values, uint32_t count)
        {
            for (uint32_t c = 0; c < count; ++c)
            {
                if (c > 0) out += L"  ";
                out += std::format(L"{:.4f}", values[c]);
            }
        }
    }

    std::wstring FormatAnalysisField(const AnalysisFieldValue& field, uint32_t maxElements)
    {
        const uint32_t stride = AnalysisFieldComponentCount(field.type);
        std::wstring text;
        if (!AnalysisFieldIsArray(field.type))
        {
            AppendComponents(text, field.components.data(), (std::min)(stride, 4u));
            return text;
        }

        const uint32_t count = stride > 0 ? static_cast<uint32_t>(field.arrayData.size()) / stride : 0;
        text = std::format(L"[{} elements]", count);
        if (maxElements == 0) return text;

        text += L"\n";
        const uint32_t shown = (std::min)(count, maxElements);
        for (uint32_t i = 0; i < shown; ++i)
        {
            text += std::format(L"  [{}] ", i);
            AppendComponents(text, field.arrayData.data() + static_cast<size_t>(i) * stride, stride);
            text += L"\n";
        }
        if (count > shown)
            text += std::format(L"  ... ({} more)\n", count - shown);
        return text;
    }

    std::wstring FormatPropertyValue(const PropertyValue& value, int decimals)
    {
        using winrt::Windows::Foundation::Numerics::float2;
        using winrt::Windows::Foundation::Numerics::float3;
        using winrt::Windows::Foundation::Numerics::float4;
        return std::visit([decimals](const auto& v) -> std::wstring
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>)
                return Fixed(v, decimals);
            else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                return std::to_wstring(v);
            else if constexpr (std::is_same_v<T, bool>)
                return v ? L"true" : L"false";
            else if constexpr (std::is_same_v<T, std::wstring>)
                return v;
            else if constexpr (std::is_same_v<T, float2>)
                return Fixed(v.x, decimals) + L", " + Fixed(v.y, decimals);
            else if constexpr (std::is_same_v<T, float3>)
                return Fixed(v.x, decimals) + L", " + Fixed(v.y, decimals) + L", " + Fixed(v.z, decimals);
            else if constexpr (std::is_same_v<T, float4>)
                return Fixed(v.x, decimals) + L", " + Fixed(v.y, decimals) + L", " +
                       Fixed(v.z, decimals) + L", " + Fixed(v.w, decimals);
            else if constexpr (std::is_same_v<T, std::vector<float>>)
                return std::format(L"[{} floats]", v.size());
            else
                return L"(matrix)";
        }, value);
    }

    std::optional<std::wstring> EnumLabelFor(const EffectNode& node, std::wstring_view parameter, float value)
    {
        if (!node.customEffect.has_value()) return std::nullopt;
        for (const auto& p : node.customEffect->parameters)
        {
            if (p.name != parameter || p.enumLabels.empty()) continue;
            const auto index = static_cast<uint32_t>(value + 0.5f);
            if (index < p.enumLabels.size()) return p.enumLabels[index];
            return std::nullopt;
        }
        return std::nullopt;
    }
}
