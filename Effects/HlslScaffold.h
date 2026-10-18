#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Effects
{
    // The starting HLSL for a custom effect: the engine headers, a cbuffer
    // of its parameters (specialised options through SHADERLAB_OPTION), its
    // inputs and an entry point of the right shape for its shader type -- for
    // a pixel shader, SV_POSITION, SCENE_POSITION and one TEXCOORD per input.
    // Every scaffold compiles as generated.
    std::wstring GenerateEffectScaffold(const Graph::CustomEffectDefinition& def);
}
