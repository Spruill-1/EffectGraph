#pragma once

#include "pch_engine.h"
#include "../../Graph/EffectNode.h"
#include "../GpuBindingPlan.h"

// The parts of a D3D11 compute dispatch that are plain functions of the
// shader's reflection and the node's properties: cbuffer packing, output size
// from properties and thread-group counts. Nothing here needs a device.
namespace ShaderLab::Rendering::Evaluator
{
    struct ReflectedCbVar
    {
        std::wstring             name;
        UINT                     offset{ 0 };
        D3D_SHADER_VARIABLE_TYPE type{ D3D_SVT_FLOAT };
        UINT                     cols{ 1 };
    };

    // A compute shader's parameter cbuffer (the one at b0) and [numthreads].
    struct ReflectedComputeCb
    {
        UINT                        sizeBytes{ 0 };
        std::vector<ReflectedCbVar> vars;
        UINT                        groupX{ 0 }, groupY{ 0 }, groupZ{ 0 };
    };

    // Reflects `bytecode`; empty when reflection fails.
    ReflectedComputeCb ReflectComputeConstants(const std::vector<uint8_t>& bytecode);

    // Bytes the runner places after its own Width/Height (the first 8 bytes of
    // the cbuffer). Each variable takes the property of the same name, typed
    // to its HLSL slot; `_SLIdx_<param>` takes the field index the binding
    // plan serves that parameter from. Empty when the cbuffer holds nothing
    // past Width/Height.
    std::vector<BYTE> PackComputeConstants(
        const ReflectedComputeCb& cb,
        const std::map<std::wstring, Graph::PropertyValue>& properties,
        const GpuBindingPlan& plan);

    // An image output size the properties state outright: OutputWidth +
    // OutputHeight (both at least 1), else the definition's square size
    // parameter (at least 64). 0 x 0 when neither is set.
    D2D1_SIZE_U ComputeOutputSizeFromProperties(
        const Graph::CustomEffectDefinition& def,
        const std::map<std::wstring, Graph::PropertyValue>& properties);

    // Thread groups covering a width x height output one thread per pixel,
    // for a PerOutputPixel shader; nullopt keeps the bridge's (1,1,1). The
    // group size comes from the shader's [numthreads] when reflection has
    // it, else from the definition.
    std::optional<std::array<UINT32, 3>> PerPixelDispatchGroups(
        const Graph::CustomEffectDefinition& def, const ReflectedComputeCb& reflected,
        UINT32 width, UINT32 height);
}
