#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"
#include "BytecodeCache.h"

// Shader variants: the single owner of how an effect's variants are named,
// keyed and compiled.
//
// Two things select a variant. Each gpu-bindable parameter has a binding
// mode (cbuffer, structured buffer, or texture), packed two bits per
// parameter. Option specialisation fixes `specialize` parameters at compile
// time: a parameter qualifies when it is marked `specialize`, has enumLabels
// and is not gpuBindable. The first cMaxSpecializedOptions that qualify are
// used, in declaration order. A value that is not an exact option selects the
// generic build, which reads the option from the cbuffer.
namespace ShaderLab::Effects
{
    // How a gpu-bindable parameter receives its value. The numbers are the
    // values of the _SLPARAM_<name>_GPU macro in shaderlab_params.hlsli.
    enum class GpuBindingMode : uint8_t
    {
        CBuffer          = 0,   // the CPU writes the value into the cbuffer
        StructuredBuffer = 1,   // a D3D11 compute consumer binds the producer's SRV
        Texture          = 2,   // a pixel-shader consumer reads it from an input image
    };

    // Two bits per parameter in a uint32: parameters past this stay cbuffer-bound.
    inline constexpr size_t cMaxGpuBindableParams = 16;

    // The mode of gpu-bindable parameter `index` in packed bits. The unused
    // value 3 reads as CBuffer, which is always correct if slower.
    inline GpuBindingMode BindingModeAt(uint32_t bits, size_t index) noexcept
    {
        if (index >= cMaxGpuBindableParams) return GpuBindingMode::CBuffer;
        const uint32_t mode = (bits >> (2u * index)) & 3u;
        return mode <= 2u ? static_cast<GpuBindingMode>(mode) : GpuBindingMode::CBuffer;
    }

    // `bits` with gpu-bindable parameter `index` set to `mode`.
    inline uint32_t WithBindingMode(uint32_t bits, size_t index, GpuBindingMode mode) noexcept
    {
        if (index >= cMaxGpuBindableParams) return bits;
        const uint32_t shift = 2u * static_cast<uint32_t>(index);
        return (bits & ~(3u << shift)) | (static_cast<uint32_t>(mode) << shift);
    }

    // Packs one mode per gpu-bindable parameter, in declaration order.
    uint32_t PackBindingModes(std::span<const GpuBindingMode> modes);

    // UTF-8 names of the gpu-bindable parameters, in declaration order.
    std::vector<std::string> GpuBindableNames(const Graph::CustomEffectDefinition& def);

    // UTF-8 names of the specialised parameters, in option key order.
    std::vector<std::string> SpecializedOptionNames(const Graph::CustomEffectDefinition& def);

    // The option key a node's values select (0 = generic).
    uint64_t OptionKeyFor(const Graph::CustomEffectDefinition& def,
                                        const std::map<std::wstring, Graph::PropertyValue>& values);

    // Every fully specialised option key. Empty when nothing is specialised
    // or there are more than cMaxOptionCombinations combinations.
    inline constexpr size_t cMaxOptionCombinations = 64;
    std::vector<uint64_t> AllOptionKeys(const Graph::CustomEffectDefinition& def);

    // Number of fully specialised combinations, 0 when nothing is
    // specialised. Stops multiplying once past a million.
    size_t OptionCombinationCount(const Graph::CustomEffectDefinition& def);

    using MacroPair = std::pair<std::string, std::string>;

    // The (name, value) macros one variant compiles with: _SLPARAM_<name>_GPU
    // for every gpu-bindable parameter, and _SLOPT_<name>_MODE plus
    // _SLOPT_<name> for every specialised option.
    std::vector<MacroPair> BuildVariantMacros(
        const std::vector<std::string>& gpuNames, uint32_t bindingBits,
        const std::vector<std::string>& optionNames, uint64_t optionKey);

    // SL_FIELD_<name> = the Result[] index (pixel offset) of each analysis
    // field, so a shader writes Result[SL_FIELD_Max] rather than a literal
    // that must match the field list by hand. A character outside
    // [A-Za-z0-9_] becomes '_'; a name that then repeats is left out.
    // Every compile of a definition gets these.
    std::vector<MacroPair> AnalysisFieldMacros(const Graph::CustomEffectDefinition& def);

    // Macros for the generic build: every gpu-bindable parameter in cbuffer
    // mode, every option read at run time, and the analysis field indices.
    std::vector<MacroPair> GenericVariantMacros(const Graph::CustomEffectDefinition& def);

    // The cache request for one variant of a definition. `canonicalHlsl` is
    // CanonicalizeHlslSource(def.hlslSource); callers building several
    // variants canonicalise once. The baseline is bindingBits 0, optionKey 0.
    BytecodeCompileRequest MakeVariantRequest(
        const Graph::CustomEffectDefinition& def, std::string canonicalHlsl,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target,
        uint32_t bindingBits, uint64_t optionKey);
    BytecodeCompileRequest MakeVariantRequest(
        const Graph::CustomEffectDefinition& def,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target,
        uint32_t bindingBits, uint64_t optionKey);

    // What a node's compiles are filed under in the bytecode cache: the
    // built-in effect id and version, or the node's name and 1 for an effect
    // authored in a graph. Every compile of one node uses the same identity.
    struct CompileIdentity
    {
        std::wstring effectId;
        uint32_t effectVersion{ 1 };
    };
    CompileIdentity CompileIdentityOf(const Graph::EffectNode& node);

    // The shader target a definition compiles to: ps_5_0 or cs_5_0.
    std::string ShaderTargetFor(const Graph::CustomEffectDefinition& def);

    // Queues the baseline plus one variant per gpu-bindable parameter in the
    // target's GPU mode, so a binding wired later finds its shader warm.
    // Idempotent. An effect with specialised options is skipped: its option
    // variants are precompiled for the binding shape in use.
    void PrecompileBindingShapes(
        BytecodeCache& cache, const Graph::CustomEffectDefinition& def,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target);
}
