#pragma once

#include "pch_engine.h"
#include "PropertyValue.h"

#include <string_view>

namespace ShaderLab::Graph
{
    struct CustomEffectDefinition;

    // Effect ids that code has to name. Saved graphs carry these strings, so
    // they never change; a display name may (Math Expression is shown as
    // "Numeric Expression").
    inline constexpr std::wstring_view cClockEffectId          = L"Clock";
    inline constexpr std::wstring_view cMathExpressionEffectId = L"Math Expression";
    inline constexpr std::wstring_view cRandomEffectId         = L"Random";
    inline constexpr std::wstring_view cWorkingSpaceEffectId   = L"Working Space";

    // cbuffer properties the evaluator writes itself (InjectedConstants).
    inline constexpr std::wstring_view cInputMaskProperty = L"InputMask";
    inline constexpr std::wstring_view cOutputWProperty   = L"OutputW";
    inline constexpr std::wstring_view cOutputHProperty   = L"OutputH";

    // What the evaluator does with a custom-effect node. Saved as a string tag.
    enum class NodeKind : uint8_t
    {
        Shader            = 0,  // compiles and runs hlslSource
        ConstantParameter = 1,  // publishes its properties as analysis fields
        Clock             = 2,  // publishes Time / Progress from its clock state
        Expression        = 3,  // publishes Result of its Expression property
        Random            = 4,  // publishes Result, a hash of its Seed property
        WorkingSpace      = 5,  // a ConstantParameter the host fills from the display
    };

    // How the evaluator sizes a D3D11 compute dispatch with an image output.
    // Saved as a string tag.
    enum class DispatchShape : uint8_t
    {
        PerOutputPixel = 0,  // ceil(output / [numthreads]) groups, one thread per output pixel
        SingleGroup    = 1,  // one group; the shader loops over its input itself
        ReduceGroups   = 2,  // the runner sizes it: a SHADERLAB reduction over scratch
        ImagePass      = 3,  // the runner sizes it: SHADERLAB_IMAGE_PASS, two passes
    };

    // A fixed output size read from a float property: sizeParam x columns
    // by sizeParam x rows. Pixel shaders apply it when sizeParam names a
    // float property, and a zero-input pixel shader falls back to 512 x 512.
    // D3D11 compute uses it as a square size of at least 64.
    struct OutputExtent
    {
        std::wstring sizeParam;
        uint32_t columns{ 1 };
        uint32_t rows{ 1 };

        bool operator==(const OutputExtent&) const = default;
    };

    // cbuffer values the evaluator writes rather than the user. Saved as an
    // array of string tags.
    enum class InjectedConstants : uint32_t
    {
        None       = 0,
        InputMask  = 1u << 0,  // InputMask: bit i set when pin i carries an image
        OutputSize = 1u << 1,  // OutputW / OutputH, and ImageAW..ImageDH when declared
    };

    constexpr InjectedConstants operator|(InjectedConstants a, InjectedConstants b)
    {
        return static_cast<InjectedConstants>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool HasInjected(InjectedConstants set, InjectedConstants flag)
    {
        return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
    }

    const wchar_t* NodeKindTag(NodeKind kind);
    std::optional<NodeKind> NodeKindFromTag(std::wstring_view tag);
    const wchar_t* DispatchShapeTag(DispatchShape shape);
    std::optional<DispatchShape> DispatchShapeFromTag(std::wstring_view tag);
    std::vector<std::wstring> InjectedConstantTags(InjectedConstants set);
    std::optional<InjectedConstants> InjectedConstantFromTag(std::wstring_view tag);

    // True when the evaluator publishes the node's analysis fields itself
    // instead of running a shader.
    constexpr bool IsParameterKind(NodeKind kind) { return kind != NodeKind::Shader; }

    // True when the evaluator, not the runner, sizes the dispatch from
    // the output and the shader's [numthreads].
    constexpr bool TilesPerOutputPixel(DispatchShape shape) { return shape == DispatchShape::PerOutputPixel; }

    // The contracts a definition saved before they were declared implies:
    // the kind from the effect id, the dispatch from the thread-group size
    // and DiagramSize / OutputSize, the output extent and injected values
    // from the property names. `properties` is the node's (parameters and
    // hidden values).
    NodeKind DeriveNodeKind(const CustomEffectDefinition& def);
    DispatchShape DeriveDispatchShape(const CustomEffectDefinition& def);
    OutputExtent DeriveOutputExtent(const CustomEffectDefinition& def,
        const std::map<std::wstring, PropertyValue>& properties);
    InjectedConstants DeriveInjectedConstants(
        const std::map<std::wstring, PropertyValue>& properties);

    // Sets all four on a definition built outside the effect catalog (the
    // Effect Designer, a blank custom node, an MCP recompile of an authored
    // effect). Pass the node's own property map, hidden values included: a
    // map of parameter defaults alone loses InputMask / OutputW / OutputH.
    void DeriveContracts(CustomEffectDefinition& def,
        const std::map<std::wstring, PropertyValue>& properties);
}
