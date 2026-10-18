#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"
#include "../Effects/ShaderVariants.h"

#include <functional>

// Which property bindings a GPU lane serves. Every decision about GPU binding
// -- the shader variant a consumer compiles, the producer SRV or texture it is
// handed, and whether its producer still needs a CPU readback -- reads one
// plan built here, so the lanes and the readback skip cannot disagree.
//
//   lane 1  D3D11 compute consumer: binds the producer's analysis SRV at a t-slot.
//   lane 3  pixel-shader consumer: reads the producer's analysis row as an
//           input image, since Direct2D binds a pixel shader's inputs only.
//
// Anything else stays on the cbuffer path, which is the reference.
namespace ShaderLab::Rendering
{
    // One binding a GPU lane serves.
    struct GpuBindingEntry
    {
        std::wstring            paramName;
        uint32_t                gpuBindableIndex{ 0 };   // position among the gpu-bindable parameters
        Effects::GpuBindingMode mode{ Effects::GpuBindingMode::CBuffer };
        uint32_t                producerId{ 0 };
        uint32_t                fieldIndex{ 0 };         // float4 index in the producer's analysis row
    };

    // The GPU-served bindings of one consumer.
    struct GpuBindingPlan
    {
        std::vector<GpuBindingEntry> entries;
        uint32_t bits{ 0 };   // packed modes, the variant to compile (ShaderVariants.h)

        const GpuBindingEntry* Find(std::wstring_view paramName) const
        {
            for (const auto& entry : entries)
                if (entry.paramName == paramName) return &entry;
            return nullptr;
        }
    };

    // The lane a consumer binds through: StructuredBuffer for a D3D11 compute
    // node, Texture for a pixel shader, CBuffer (no lane) for anything else.
    Effects::GpuBindingMode GpuLaneFor(const Graph::EffectNode& consumer);

    // The float4 index a lane loads for a binding from `producer`'s analysis
    // row, or nullopt when no lane can serve it. A lane loads one float4 and
    // swizzles it by the parameter type (.x, .xy, ...), so destination
    // component i must come from component i of one element of the source
    // field; a FloatArray element is servable only from a pixel's .x.
    std::optional<uint32_t> GpuBindingFieldIndex(
        const Graph::PropertyBinding& binding, const Graph::EffectNode& producer);

    // Plans which of `consumer`'s bindings its lane serves. A binding qualifies
    // when GPU bindings are on, its parameter is gpu-bindable, its producer is
    // a D3D11 compute node and GpuBindingFieldIndex can address it.
    // `producerReady(producerId)` says whether that producer's GPU output
    // exists for this use; leave it empty for the shape the consumer will run
    // once its producers have dispatched.
    GpuBindingPlan PlanGpuBindings(
        const Graph::EffectNode& consumer,
        const Graph::GraphModel& graph,
        bool gpuBindingsEnabled,
        const std::function<bool(uint32_t producerId)>& producerReady = {});
}
