#pragma once

#include "../Effects/ShaderLabEffects.h"

// The source of ShaderLab's built-in shader effects. Only EffectGraphCompile
// (which generates a header per effect from it) and the tests link this
// library; programs get the effects from the generated headers.
namespace ShaderLab::Effects::BuiltIn
{
    // Every built-in shader effect, in the order of Effects::BuiltInEffectList,
    // without the C++ hooks the engine attaches when one registers.
    std::vector<ShaderLabEffectDescriptor> ShaderEffects();
}
