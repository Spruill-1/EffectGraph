#pragma once

#include "pch_engine.h"
#include "../../Graph/EffectNode.h"

// Lane-3 pin layout of a custom D2D effect.
//
// Direct2D maps a transform's INPUTS to t0, t1, ... so a pixel shader that
// receives analysis values as textures needs real input pins for them. The
// layout is fixed and shared by everything that reasons about it --
// registration, creation, wiring and the shader's own SHADERLAB_GPU_BUFFER
// slot numbers must all agree, and a mismatch shows up as a shader sampling
// the wrong texture rather than as an error:
//
//     pin 0 .. G-1   geometric image inputs
//     pin G .. N-1   lookup inputs (read at computed coordinates)
//     pin N + i      gpu-bindable parameter i
//
// Pins are reserved for EVERY gpu-bindable parameter whether or not it is
// bound this frame, because the input count is fixed at effect registration
// and varying it would mean re-registering the CLSID whenever a binding is
// added. Unbound pins hold a placeholder, which the shader never samples (it
// is compiled in cbuffer mode for that parameter). Effects declaring no
// gpu-bindable parameters are unaffected.
namespace ShaderLab::Rendering::Evaluator
{
    // Declared inputs, at least one: D2D needs an input to size a generator.
    inline uint32_t DeclaredInputCount(const Graph::CustomEffectDefinition& def)
    {
        return static_cast<uint32_t>((std::max)(size_t(1), def.inputNames.size()));
    }

    // Declared inputs that are IMAGES -- the ones that decide where the effect
    // draws and that D2D may hand over one tile at a time. The rest are lookup
    // tables (CustomEffectDefinition::lookupInputCount). At least one
    // geometric input always remains.
    inline uint32_t GeometricInputCount(const Graph::CustomEffectDefinition& def)
    {
        const uint32_t declared = DeclaredInputCount(def);
        const uint32_t lookups  = (std::min)(def.lookupInputCount, declared - 1);
        return declared - lookups;
    }

    inline uint32_t GpuBindablePinCount(const Graph::CustomEffectDefinition& def)
    {
        uint32_t count = 0;
        for (const auto& param : def.parameters) if (param.gpuBindable) ++count;
        return count;
    }

    // Total D2D input count. Only pixel shaders reserve analysis pins: a
    // compute consumer binds its upstream SRV directly at a t-slot.
    inline uint32_t TotalInputCount(const Graph::EffectNode& node, const Graph::CustomEffectDefinition& def)
    {
        uint32_t total = DeclaredInputCount(def);
        if (node.type == Graph::NodeType::PixelShader)
            total += GpuBindablePinCount(def);
        return total;
    }
}
