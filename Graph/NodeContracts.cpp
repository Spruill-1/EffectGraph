#include "pch_engine.h"
#include "NodeContracts.h"
#include "EffectNode.h"
#include "EffectEdge.h"

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
            { DispatchShape::ReduceCells,    L"reduceCells" },
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

    namespace
    {
        bool IsD3D11ComputeNode(const EffectNode& node)
        {
            return node.customEffect.has_value() && node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;
        }

        // Image pins first, then lookup pins; at least one image pin.
        uint32_t ImagePinCount(const CustomEffectDefinition& def)
        {
            const uint32_t declared = static_cast<uint32_t>((std::max)(size_t(1), def.inputNames.size()));
            return declared - (std::min)(def.lookupInputCount, declared - 1);
        }
    }

    std::span<const GUID> RegionIdentityBuiltIns()
    {
        // Each row is one the oracle admitted; see its test before adding one.
        static const GUID cIdentity[] = {
            CLSID_D2D1ColorMatrix,
            CLSID_D2D1Brightness,
            CLSID_D2D1Contrast,
            CLSID_D2D1Exposure,
            CLSID_D2D1Grayscale,
            CLSID_D2D1HueRotation,
            CLSID_D2D1Invert,
            CLSID_D2D1Saturation,
            CLSID_D2D1Sepia,
            CLSID_D2D1TemperatureTint,
            CLSID_D2D1Posterize,
            CLSID_D2D1HdrToneMap,
            CLSID_D2D1WhiteLevelAdjustment,
            CLSID_D2D1Opacity,
            CLSID_D2D1Blend,
            CLSID_D2D1Composite,
            CLSID_D2D1AlphaMask,
        };
        return cIdentity;
    }

    bool IsRegionIdentityBuiltIn(const GUID& clsid)
    {
        return std::ranges::any_of(RegionIdentityBuiltIns(), [&](const GUID& g) { return IsEqualGUID(g, clsid); });
    }

    DirtyRegion EmittedRegion(const EffectNode& node, const DirtyRegion& region)
    {
        return IsD3D11ComputeNode(node) && !region.IsNone() ? DirtyRegion::Whole() : region;
    }

    DirtyRegion MapRegion(const EffectNode& src, const EffectNode& dst, const EffectEdge& edge, const DirtyRegion& region)
    {
        if (region.IsNone()) return region;
        // A D3D11 compute node's region is its input region; what it emits
        // changed whole.
        if (IsD3D11ComputeNode(src)) return DirtyRegion::Whole();
        if (region.IsWhole()) return region;
        if (dst.type == NodeType::Output) return region;
        // Into a D3D11 compute node's image pin the rects stay as they are,
        // in that input's texel space: the node's input region, which the
        // analysis change gate judges its dispatch by.
        // Moves change a reduction's input at their destinations only.
        if (IsD3D11ComputeNode(dst) && edge.destPin < ImagePinCount(*dst.customEffect))
            return region.WithMovesCollapsed();
        // A custom pixel shader declares TRIVIAL_SAMPLING: output pixel N
        // reads input pixel N of each image pin. Lookup pins, read at
        // computed coordinates, follow the image pins. A fixed output extent
        // (a diagram: CIE Chromaticity Plot, ICtCp Boundary) is not laid out
        // in its input's pixels. A move stays a move only through a
        // shift-invariant shader with one image pin: with a second, the
        // moved pixels would meet the other input's unmoved ones.
        if (dst.type == NodeType::PixelShader && dst.customEffect.has_value() && !dst.IsParameterNode() &&
            dst.customEffect->shaderType == CustomShaderType::PixelShader &&
            dst.customEffect->outputExtent.sizeParam.empty())
        {
            const uint32_t imagePins = ImagePinCount(*dst.customEffect);
            if (edge.destPin < imagePins)
                return dst.customEffect->shiftInvariant && imagePins == 1 ? region : region.WithMovesCollapsed();
        }
        // A per-pixel built-in Direct2D maps identically on every input.
        // Nothing says it is shift-invariant, so moves collapse.
        if (dst.type == NodeType::BuiltInEffect && dst.effectClsid.has_value() &&
            IsRegionIdentityBuiltIn(*dst.effectClsid))
            return region.WithMovesCollapsed();
        // Other built-ins, D2D compute, a lookup pin and every other pin.
        return DirtyRegion::Whole();
    }
}
