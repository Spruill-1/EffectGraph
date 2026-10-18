#pragma once

#include "pch_engine.h"
#include "../Graph/PropertyValue.h"
#include "ShaderIncludes.h"

namespace ShaderLab::Effects
{
    // D3DCompile flags for every effect shader. Part of the bytecode cache
    // key (IncludeLibraryHashOf), so changing them invalidates the cache.
    constexpr UINT cShaderCompileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    // Describes a single constant buffer variable discovered via D3DReflect.
    struct ShaderVariable
    {
        std::wstring name;
        uint32_t     offset{ 0 };       // byte offset in the constant buffer
        uint32_t     size{ 0 };          // byte size
        D3D_SHADER_VARIABLE_TYPE type{ D3D_SVT_FLOAT };
        uint32_t     rows{ 0 };
        uint32_t     columns{ 0 };
    };

    // Describes a constant buffer discovered via D3DReflect.
    struct ShaderConstantBuffer
    {
        std::wstring name;
        uint32_t     sizeBytes{ 0 };
        std::vector<ShaderVariable> variables;
    };

    // Compilation result.
    struct ShaderCompileResult
    {
        winrt::com_ptr<ID3DBlob> bytecode;      // Compiled shader blob (nullptr on failure)
        winrt::com_ptr<ID3DBlob> errors;         // Error/warning messages (may be set even on success)
        bool                     succeeded{ false };

        // Human-readable error string (empty on success).
        std::wstring ErrorMessage() const;
    };

    // Reflection result.
    struct ShaderReflectionResult
    {
        std::vector<ShaderConstantBuffer> constantBuffers;
        uint32_t                          boundResources{ 0 };  // SRV/UAV count
        uint32_t                          inputCount{ 0 };      // texture inputs
    };

    // HLSL shader compilation and reflection utilities.
    // Wraps D3DCompile and D3DReflect for use by the custom effect system.
    class ShaderCompiler
    {
    public:
        // Compile an HLSL file from disk.
        // entryPoint: e.g. "main"
        // target:     e.g. "ps_5_0" or "cs_5_0"
        static ShaderCompileResult CompileFromFile(
            const std::filesystem::path& hlslPath,
            const std::string& entryPoint = "main",
            const std::string& target = "ps_5_0");

        // Compile HLSL source from a string.
        static ShaderCompileResult CompileFromString(
            const std::string& hlslSource,
            const std::string& sourceName,
            const std::string& entryPoint = "main",
            const std::string& target = "ps_5_0");

        // Compile HLSL source with explicit preprocessor macros. Used
        // by the GPU-binding feature to inject _SLPARAM_<name>_GPU=0|1|2
        // sentinels per gpuBindable parameter. Includes resolve through
        // ShaderLabIncludeHandler(); other includes are not supported
        // (D3D_COMPILE_STANDARD_FILE_INCLUDE is intentionally bypassed
        // since all ShaderLab shaders are in-memory strings).
        //
        // `macros` is a flat list of {name, definition} pairs, NOT
        // null-terminated -- the ShaderCompiler appends the terminator
        // internally. Both name and definition strings must outlive
        // the call.
        struct MacroDef { const char* name; const char* definition; };
        static ShaderCompileResult CompileFromString(
            const std::string& hlslSource,
            const std::string& sourceName,
            const std::string& entryPoint,
            const std::string& target,
            const std::vector<MacroDef>& macros);

        // Reflect a compiled shader to discover constant buffers and bindings.
        static ShaderReflectionResult Reflect(ID3DBlob* bytecode);
        static ShaderReflectionResult Reflect(const std::vector<uint8_t>& bytecode);
    };

    // ---- Typed PropertyValue -> cbuffer pack ---------------------------------
    //
    // Packs a single PropertyValue into a constant-buffer byte slot,
    // converting via the declared HLSL type so that:
    //   * float  PropertyValue -> uint cbuffer slot: static_cast<uint32_t>
    //   * float  PropertyValue -> int  cbuffer slot: static_cast<int32_t>
    //   * float  PropertyValue -> bool cbuffer slot: BOOL(value > 0.5)
    //   * uint/int/bool PropertyValue -> matching slot: direct copy
    //   * float vector PropertyValue -> matching N-component slot: direct copy
    // so a float is never memcpy'd into a uint slot as its bit pattern. Every
    // cbuffer packer (pixel, D2D compute and D3D11 compute) goes through it.
    //
    // Returns true if any bytes were written. `dest` may not be null;
    // `remaining` is the remaining cbuffer bytes from `dest`.
    bool PackPropertyToCBuffer(
        BYTE* dest, uint32_t remaining,
        D3D_SHADER_VARIABLE_TYPE hlslType,
        uint32_t hlslColumns,
        const Graph::PropertyValue& value);
}
