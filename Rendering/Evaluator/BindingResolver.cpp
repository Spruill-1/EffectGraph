#include "pch_engine.h"
#include "BindingResolver.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        namespace Numerics = winrt::Windows::Foundation::Numerics;

        // A resolved binding must never carry a non-finite value into
        // node.properties: that map is what /graph/save serialises, and a NaN
        // lands in the document as 1.#QNAN -- an .effectgraph no JSON parser
        // will load. Covers every float-bearing alternative.
        bool IsFinitePropertyValue(const PropertyValue& v)
        {
            if (const auto* f = std::get_if<float>(&v))   return std::isfinite(*f);
            if (const auto* f2 = std::get_if<Numerics::float2>(&v)) return std::isfinite(f2->x) && std::isfinite(f2->y);
            if (const auto* f3 = std::get_if<Numerics::float3>(&v))
                return std::isfinite(f3->x) && std::isfinite(f3->y) && std::isfinite(f3->z);
            if (const auto* f4 = std::get_if<Numerics::float4>(&v))
                return std::isfinite(f4->x) && std::isfinite(f4->y) && std::isfinite(f4->z) && std::isfinite(f4->w);
            if (const auto* vec = std::get_if<std::vector<float>>(&v))
            {
                for (float e : *vec) if (!std::isfinite(e)) return false;
                return true;
            }
            if (const auto* m = std::get_if<D2D1_MATRIX_5X4_F>(&v))
            {
                for (int i = 0; i < 20; ++i)
                    if (!std::isfinite(reinterpret_cast<const float*>(m)[i])) return false;
                return true;
            }
            return true;   // int / uint / bool / wstring cannot be non-finite
        }

        // Exact-equality compare for PropertyValue. std::variant's operator==
        // is unusable because D2D1_MATRIX_5X4_F has no operator==. Exact float
        // compare is intentional: binding sources are deterministic frame to
        // frame, so "unchanged" means bit-identical.
        bool PropertyValuesEqual(const PropertyValue& a, const PropertyValue& b)
        {
            if (a.index() != b.index()) return false;
            return std::visit([&b](const auto& av) -> bool
            {
                using T = std::decay_t<decltype(av)>;
                const T* bv = std::get_if<T>(&b);
                if (!bv) return false;
                if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    return std::memcmp(&av, bv, sizeof(T)) == 0;
                else if constexpr (std::is_same_v<T, Numerics::float2>)
                    return av.x == bv->x && av.y == bv->y;
                else if constexpr (std::is_same_v<T, Numerics::float3>)
                    return av.x == bv->x && av.y == bv->y && av.z == bv->z;
                else if constexpr (std::is_same_v<T, Numerics::float4>)
                    return av.x == bv->x && av.y == bv->y &&
                           av.z == bv->z && av.w == bv->w;
                else
                    return av == *bv;
            }, a);
        }

        // One bound component as a float.
        bool ResolveComponentSource(const ComponentSource& src, const GraphModel& graph, float& outValue)
        {
            const EffectNode* srcNode = graph.FindNode(src.sourceNodeId);
            if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
                return false;

            for (const auto& fv : srcNode->analysisOutput.fields)
            {
                if (fv.name != src.sourceFieldName) continue;

                if (AnalysisFieldIsArray(fv.type))
                {
                    uint32_t stride = AnalysisFieldComponentCount(fv.type);
                    uint32_t flatIdx = src.sourceIndex * stride + (std::min)(src.sourceComponent, stride - 1);
                    if (flatIdx < fv.arrayData.size())
                    {
                        outValue = fv.arrayData[flatIdx];
                        return true;
                    }
                }
                else
                {
                    uint32_t comp = (std::min)(src.sourceComponent, 3u);
                    outValue = fv.components[comp];
                    return true;
                }
                return false;
            }
            return false;
        }

        // A vector property with each bound component replaced; nullopt when
        // no component resolved.
        template <typename TVector, uint32_t cComponents>
        std::optional<TVector> ResolveVector(const TVector& authored, const PropertyBinding& binding,
                                             const GraphModel& graph)
        {
            TVector result = authored;
            bool any = false;
            for (uint32_t c = 0; c < cComponents && c < binding.sources.size(); ++c)
            {
                if (!binding.sources[c].has_value()) continue;
                float v = 0;
                if (ResolveComponentSource(*binding.sources[c], graph, v))
                {
                    (&result.x)[c] = v;
                    any = true;
                }
            }
            return any ? std::optional<TVector>(result) : std::nullopt;
        }
    }

    bool ResolveBindings(const EffectNode& node, const GraphModel& graph, PropertyMap& effectiveProps)
    {
        effectiveProps = node.properties;
        if (node.propertyBindings.empty())
            return false;

        bool anyChanged = false;
        for (const auto& [propName, binding] : node.propertyBindings)
        {
            auto propIt = effectiveProps.find(propName);
            if (propIt == effectiveProps.end()) continue;

            if (binding.wholeArray)
            {
                const EffectNode* srcNode = graph.FindNode(binding.wholeArraySourceNodeId);
                if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
                    continue;
                for (const auto& fv : srcNode->analysisOutput.fields)
                {
                    if (fv.name != binding.wholeArraySourceFieldName || !AnalysisFieldIsArray(fv.type))
                        continue;
                    // A change only when the value differs: reporting every
                    // resolve as a change re-applied properties and defeated
                    // output caching on every binding consumer.
                    PropertyValue resolvedArray = fv.arrayData;
                    if (!PropertyValuesEqual(propIt->second, resolvedArray))
                    {
                        effectiveProps[propName] = std::move(resolvedArray);
                        anyChanged = true;
                    }
                    break;
                }
                continue;
            }

            if (binding.sources.empty()) continue;

            std::optional<PropertyValue> newVal;
            std::visit([&](const auto& existing)
            {
                using T = std::decay_t<decltype(existing)>;
                if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                {
                    float v = 0;
                    if (binding.sources[0].has_value() && ResolveComponentSource(*binding.sources[0], graph, v))
                        newVal = static_cast<T>(v);
                }
                else if constexpr (std::is_same_v<T, Numerics::float2>)
                {
                    if (auto r = ResolveVector<T, 2>(existing, binding, graph)) newVal = *r;
                }
                else if constexpr (std::is_same_v<T, Numerics::float3>)
                {
                    if (auto r = ResolveVector<T, 3>(existing, binding, graph)) newVal = *r;
                }
                else if constexpr (std::is_same_v<T, Numerics::float4>)
                {
                    if (auto r = ResolveVector<T, 4>(existing, binding, graph)) newVal = *r;
                }
            }, propIt->second);

            if (newVal && !PropertyValuesEqual(propIt->second, *newVal))
            {
                effectiveProps[propName] = std::move(*newVal);
                anyChanged = true;
            }
        }
        return anyChanged;
    }

    void WriteBackBindings(EffectNode& node, const PropertyMap& effectiveProps)
    {
        for (const auto& [propName, binding] : node.propertyBindings)
        {
            auto resolved = effectiveProps.find(propName);
            if (resolved == effectiveProps.end()) continue;
            if (!IsFinitePropertyValue(resolved->second))
            {
                node.runtimeError = L"binding '" + propName + L"' resolved to a non-finite value; previous kept";
                continue;
            }
            node.properties[propName] = resolved->second;
        }
    }

    bool ResolveAndWriteBackBindings(EffectNode& node, const GraphModel& graph, PropertyMap& effectiveProps)
    {
        if (!ResolveBindings(node, graph, effectiveProps))
            return false;
        WriteBackBindings(node, effectiveProps);
        return true;
    }
}
