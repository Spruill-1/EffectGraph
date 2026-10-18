#include "pch_engine.h"
#include "ParameterModel.h"

namespace ShaderLab::Graph
{
    namespace
    {
        const ParameterDefinition* ValueParameter(const EffectNode& node)
        {
            if (!node.customEffect.has_value()) return nullptr;
            for (const auto& param : node.customEffect->parameters)
                if (param.name == L"Value") return &param;
            return nullptr;
        }

        std::optional<float> ReadFloat(const EffectNode& node, const wchar_t* key)
        {
            auto it = node.properties.find(key);
            if (it == node.properties.end()) return std::nullopt;
            if (auto* value = std::get_if<float>(&it->second)) return *value;
            return std::nullopt;
        }
    }

    ParameterRange ReadParameterRange(const EffectNode& node)
    {
        ParameterRange range;
        if (const auto* value = ValueParameter(node))
        {
            range.min = value->minValue;
            range.max = value->maxValue;
            range.step = value->step;
        }
        if (node.properties.count(L"Min")) range.min = ReadFloat(node, L"Min").value_or(0.0f);
        if (node.properties.count(L"Max")) range.max = ReadFloat(node, L"Max").value_or(1.0f);
        return range;
    }

    float ParameterNormalizedValue(const EffectNode& node)
    {
        const ParameterRange range = ReadParameterRange(node);
        const float value = ReadFloat(node, L"Value").value_or(0.0f);
        const float t = range.max > range.min ? (value - range.min) / (range.max - range.min) : 0.0f;
        return std::clamp(t, 0.0f, 1.0f);
    }

    bool SetParameterNormalized(EffectNode& node, double normalized)
    {
        auto valueIt = node.properties.find(L"Value");
        if (valueIt == node.properties.end()) return false;

        const ParameterRange range = ReadParameterRange(node);
        const float t = static_cast<float>(std::clamp(normalized, 0.0, 1.0));
        float value = range.min + t * (range.max - range.min);
        if (range.step > 0.001f)
            value = std::round(value / range.step) * range.step;
        value = (std::max)(range.min, (std::min)(range.max, value));

        const float* current = std::get_if<float>(&valueIt->second);
        if (std::abs(value - (current ? *current : 0.0f)) < 0.0001f) return false;
        valueIt->second = value;
        node.dirty = true;
        return true;
    }
}
