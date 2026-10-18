#pragma once

#include "pch_engine.h"

#include <optional>
#include <span>
#include <string_view>

// The HLSL headers a shader may #include, embedded in the engine: the one
// place the compiler and the bytecode cache learn their names and text.
namespace ShaderLab::Effects
{
    struct EmbeddedInclude
    {
        std::string_view name;   // what the shader writes in #include "..."
        std::string_view text;
    };

    // shaderlab_params.hlsli, shaderlab_colormath.hlsli and
    // shaderlab_gamut.hlsli, in that order. Valid for the process lifetime.
    std::span<const EmbeddedInclude> EmbeddedIncludes();

    // The text of the embedded header called `name`, matched exactly (no
    // path), or nullopt.
    std::optional<std::string_view> FindEmbeddedInclude(std::string_view name);

    // Include handler serving EmbeddedIncludes and nothing else: shaders
    // compile from in-memory strings, so there is no directory to search.
    // Every D3DCompile of effect HLSL passes it, so all compile paths resolve
    // the same names. Never null; do not release.
    ID3DInclude* ShaderLabIncludeHandler();
}
