#include "pch_engine.h"
#include "NodeContracts.h"
#include "EffectNode.h"

namespace ShaderLab::Graph
{
    namespace
    {
        struct NodeKindName { NodeKind kind; const wchar_t* tag; };
        constexpr NodeKindName cNodeKindTags[] = {
            { NodeKind::Shader,            L"shader" },
            { NodeKind::ConstantParameter, L"parameter" },
            { NodeKind::Clock,             L"clock" },
            { NodeKind::Expression,        L"expression" },
            { NodeKind::Random,            L"random" },
            { NodeKind::WorkingSpace,      L"workingSpace" },
        };

        struct DispatchShapeName { DispatchShape shape; const wchar_t* tag; };
        constexpr DispatchShapeName cDispatchShapeTags[] = {
            { DispatchShape::PerOutputPixel, L"perOutputPixel" },
            { DispatchShape::SingleGroup,    L"singleGroup" },
            { DispatchShape::ReduceGroups,   L"reduceGroups" },
            { DispatchShape::ImagePass,      L"imagePass" },
        };

        struct InjectedName { InjectedConstants flag; const wchar_t* tag; };
        constexpr InjectedName cInjectedTags[] = {
            { InjectedConstants::InputMask,  L"inputMask" },
            { InjectedConstants::OutputSize, L"outputSize" },
        };

        bool IsSquareSizeParam(const std::wstring& name)
        {
            return name == L"DiagramSize" || name == L"OutputSize";
        }

        bool HasFloat(const std::map<std::wstring, PropertyValue>& properties, const wchar_t* name)
        {
            auto it = properties.find(name);
            return it != properties.end() && std::holds_alternative<float>(it->second);
        }
    }

    const wchar_t* NodeKindTag(NodeKind kind)
    {
        for (const auto& entry : cNodeKindTags)
            if (entry.kind == kind) return entry.tag;
        return L"shader";
    }

    std::optional<NodeKind> NodeKindFromTag(std::wstring_view tag)
    {
        for (const auto& entry : cNodeKindTags)
            if (tag == entry.tag) return entry.kind;
        return std::nullopt;
    }

    const wchar_t* DispatchShapeTag(DispatchShape shape)
    {
        for (const auto& entry : cDispatchShapeTags)
            if (entry.shape == shape) return entry.tag;
        return L"perOutputPixel";
    }

    std::optional<DispatchShape> DispatchShapeFromTag(std::wstring_view tag)
    {
        for (const auto& entry : cDispatchShapeTags)
            if (tag == entry.tag) return entry.shape;
        return std::nullopt;
    }

    std::vector<std::wstring> InjectedConstantTags(InjectedConstants set)
    {
        std::vector<std::wstring> tags;
        for (const auto& entry : cInjectedTags)
            if (HasInjected(set, entry.flag)) tags.emplace_back(entry.tag);
        return tags;
    }

    std::optional<InjectedConstants> InjectedConstantFromTag(std::wstring_view tag)
    {
        for (const auto& entry : cInjectedTags)
            if (tag == entry.tag) return entry.flag;
        return std::nullopt;
    }

    NodeKind DeriveNodeKind(const CustomEffectDefinition& def)
    {
        // A parameter node is a pixel-shader definition with no HLSL that only
        // publishes typed fields. A blank D3D11 compute node is a shader
        // waiting for its HLSL.
        if (def.shaderType == CustomShaderType::D3D11ComputeShader ||
            !def.hlslSource.empty() || def.analysisOutputType != AnalysisOutputType::Typed ||
            def.analysisFields.empty())
            return NodeKind::Shader;
        const std::wstring_view id = def.shaderLabEffectId;
        if (id == cClockEffectId)          return NodeKind::Clock;
        if (id == cMathExpressionEffectId) return NodeKind::Expression;
        if (id == cRandomEffectId)         return NodeKind::Random;
        if (id == cWorkingSpaceEffectId)   return NodeKind::WorkingSpace;
        return NodeKind::ConstantParameter;
    }

    DispatchShape DeriveDispatchShape(const CustomEffectDefinition& def)
    {
        // A group of 256 threads or more, or a DiagramSize / OutputSize
        // parameter, meant one group; a small 2D group meant per-pixel tiles.
        const uint32_t threads = def.threadGroupX * def.threadGroupY;
        const bool namesFixedSize = std::ranges::any_of(def.parameters,
            [](const ParameterDefinition& p) { return IsSquareSizeParam(p.name); });
        const bool tiles = def.threadGroupX > 0 && def.threadGroupY > 0 &&
            threads < 256 && !namesFixedSize && threads >= 4 && def.threadGroupY >= 2;
        return tiles ? DispatchShape::PerOutputPixel : DispatchShape::SingleGroup;
    }

    OutputExtent DeriveOutputExtent(const CustomEffectDefinition& def,
        const std::map<std::wstring, PropertyValue>& properties)
    {
        if (def.shaderType == CustomShaderType::D3D11ComputeShader)
        {
            // The first DiagramSize / OutputSize parameter, whatever its type.
            for (const auto& p : def.parameters)
                if (IsSquareSizeParam(p.name)) return { p.name };
            return {};
        }
        if (def.shaderType != CustomShaderType::PixelShader)
            return {};

        if (HasFloat(properties, L"DiagramSize")) return { L"DiagramSize" };
        if (HasFloat(properties, L"OutputSize"))  return { L"OutputSize" };
        // A Color Checker's patches: 6 columns by 4 rows.
        if (HasFloat(properties, L"PatchSize"))   return { L"PatchSize", 6, 4 };
        if (def.inputNames.empty())
        {
            // A generator: the first float property whose name has "Size".
            for (const auto& [key, value] : properties)
            {
                const bool namesSize = key.find(L"Size") != std::wstring::npos ||
                                       key.find(L"size") != std::wstring::npos;
                if (namesSize && std::holds_alternative<float>(value))
                    return { key };
            }
        }
        return {};
    }

    InjectedConstants DeriveInjectedConstants(const std::map<std::wstring, PropertyValue>& properties)
    {
        InjectedConstants injected = InjectedConstants::None;
        if (properties.contains(std::wstring(cInputMaskProperty)))
            injected = injected | InjectedConstants::InputMask;
        if (properties.contains(std::wstring(cOutputWProperty)) || properties.contains(std::wstring(cOutputHProperty)))
            injected = injected | InjectedConstants::OutputSize;
        return injected;
    }

    void DeriveContracts(CustomEffectDefinition& def, const std::map<std::wstring, PropertyValue>& properties)
    {
        def.nodeKind = DeriveNodeKind(def);
        def.dispatchShape = DeriveDispatchShape(def);
        def.outputExtent = DeriveOutputExtent(def, properties);
        def.injectedConstants = DeriveInjectedConstants(properties);
    }
}
