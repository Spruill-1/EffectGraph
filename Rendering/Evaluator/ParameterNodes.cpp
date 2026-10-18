#include "pch_engine.h"
#include "ParameterNodes.h"
#include "../MathExpression.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        float FloatProperty(const EffectNode& node, const wchar_t* name, float fallback)
        {
            auto it = node.properties.find(name);
            if (it != node.properties.end())
                if (const auto* value = std::get_if<float>(&it->second)) return *value;
            return fallback;
        }

        std::vector<AnalysisFieldValue> ClockFields(const EffectNode& node)
        {
            const float startTime = FloatProperty(node, L"StartTime", 0.0f);
            const float stopTime = FloatProperty(node, L"StopTime", 10.0f);
            double duration = static_cast<double>(stopTime - startTime);
            if (duration <= 0.0) duration = 1.0;

            // Quantise to the emit rate with the same rule the tick uses to
            // decide whether to dirty; otherwise a consumer woken for a tick
            // could read a value from between ticks.
            const float updateRate = FloatProperty(node, L"UpdateRate", 0.0f);
            double quantized = node.clockTime;
            if (updateRate > 0.0f)
            {
                const double step = 1.0 / static_cast<double>(updateRate);
                quantized = std::floor(node.clockTime / step) * step;
            }

            const float currentTime = startTime + static_cast<float>(quantized);
            const float progress = static_cast<float>(quantized / duration);

            std::vector<AnalysisFieldValue> fields;
            for (const auto& fd : node.customEffect->analysisFields)
            {
                AnalysisFieldValue fv;
                fv.name = fd.name;
                fv.type = fd.type;
                if (fd.name == L"Time") fv.components[0] = currentTime;
                else if (fd.name == L"Progress") fv.components[0] = progress;
                fields.push_back(std::move(fv));
            }
            return fields;
        }

        // Evaluates the Expression property with each declared float
        // parameter (A, B, C, ...) bound by name.
        std::vector<AnalysisFieldValue> ExpressionFields(EffectNode& node)
        {
            std::wstring expr;
            auto eIt = node.properties.find(L"Expression");
            if (eIt != node.properties.end())
                if (auto* s = std::get_if<std::wstring>(&eIt->second)) expr = *s;

            std::vector<std::wstring> nameStrs;
            std::vector<const wchar_t*> namePtrs;
            std::vector<float> vals;
            const auto& params = node.customEffect->parameters;
            nameStrs.reserve(params.size());
            namePtrs.reserve(params.size());
            vals.reserve(params.size());
            for (const auto& p : params)
            {
                if (p.name == L"Expression") continue;
                if (p.typeName != L"float") continue;
                nameStrs.push_back(p.name);
                vals.push_back(FloatProperty(node, p.name.c_str(), 0.0f));
            }
            for (auto& n : nameStrs) namePtrs.push_back(n.c_str());

            float result = 0.0f;
            std::wstring err;
            const bool ok = EvaluateMathExpression(expr, namePtrs.data(), vals.data(), vals.size(), result, &err);
            node.runtimeError = ok ? std::wstring{} : err;

            AnalysisFieldValue fv;
            fv.name = L"Result";
            fv.type = AnalysisFieldType::Float;
            fv.components[0] = result;
            return { std::move(fv) };
        }

        // Hashes the Seed float into a uniform float in [0, 1): a
        // SplitMix64-style mixer on its bit pattern, so equal seeds give
        // equal values on every build and architecture.
        std::vector<AnalysisFieldValue> RandomFields(const EffectNode& node)
        {
            const float seed = FloatProperty(node, L"Seed", 0.0f);

            uint32_t bits = 0;
            std::memcpy(&bits, &seed, sizeof(bits));
            uint64_t z = static_cast<uint64_t>(bits) * 0x9E3779B97F4A7C15ULL + 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z =  z ^ (z >> 31);
            // The top 24 bits keep a float's full mantissa precision.
            const uint32_t mantissa = static_cast<uint32_t>(z >> 40);

            AnalysisFieldValue fv;
            fv.name = L"Result";
            fv.type = AnalysisFieldType::Float;
            fv.components[0] = static_cast<float>(mantissa) / 16777216.0f;
            return { std::move(fv) };
        }

        // Copies each field's property into it. Host-managed nodes (Working
        // Space) store float2/3/4, bool, int and uint under field names.
        std::vector<AnalysisFieldValue> ConstantFields(const EffectNode& node)
        {
            namespace Numerics = winrt::Windows::Foundation::Numerics;
            std::vector<AnalysisFieldValue> fields;
            for (const auto& fd : node.customEffect->analysisFields)
            {
                AnalysisFieldValue fv;
                fv.name = fd.name;
                fv.type = fd.type;
                auto propIt = node.properties.find(fd.name);
                if (propIt != node.properties.end())
                {
                    const auto& pv = propIt->second;
                    if (auto* f = std::get_if<float>(&pv))
                        fv.components[0] = *f;
                    else if (auto* b = std::get_if<bool>(&pv))
                        fv.components[0] = *b ? 1.0f : 0.0f;
                    else if (auto* i = std::get_if<int32_t>(&pv))
                        fv.components[0] = static_cast<float>(*i);
                    else if (auto* u = std::get_if<uint32_t>(&pv))
                        fv.components[0] = static_cast<float>(*u);
                    else if (auto* v2 = std::get_if<Numerics::float2>(&pv))
                    {
                        fv.components[0] = v2->x;
                        fv.components[1] = v2->y;
                    }
                    else if (auto* v3 = std::get_if<Numerics::float3>(&pv))
                    {
                        fv.components[0] = v3->x;
                        fv.components[1] = v3->y;
                        fv.components[2] = v3->z;
                    }
                    else if (auto* v4 = std::get_if<Numerics::float4>(&pv))
                    {
                        fv.components[0] = v4->x;
                        fv.components[1] = v4->y;
                        fv.components[2] = v4->z;
                        fv.components[3] = v4->w;
                    }
                }
                fields.push_back(std::move(fv));
            }
            return fields;
        }
    }

    std::vector<AnalysisFieldValue> ParameterNodeFields(EffectNode& node)
    {
        switch (node.customEffect->nodeKind)
        {
        case NodeKind::Clock:      return ClockFields(node);
        case NodeKind::Expression: return ExpressionFields(node);
        case NodeKind::Random:     return RandomFields(node);
        default:                   return ConstantFields(node);
        }
    }
}
