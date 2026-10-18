#pragma once

#include <cstdint>

namespace ShaderLab::Rendering
{
    // How one GraphEvaluator renders. Each evaluator holds its own copy:
    // GraphEvaluator::SetOptions pins it, and until then the evaluator follows
    // the process defaults kept by Performance::. The evaluator reads the
    // value once at the start of each Normal Evaluate and holds it for the
    // whole frame, so a change made mid-frame takes effect on the next one.
    struct EvaluatorOptions
    {
        // Serve a gpuBindable parameter bound to a compute node's analysis
        // output on the GPU (a structured-buffer SRV for compute consumers,
        // an analysis texture for pixel-shader consumers) instead of through
        // a CPU readback. Off: every binding goes through the cbuffer.
        bool gpuBindings{ true };

        // Skip the CPU Map of a compute node's analysis buffer when nothing
        // needs the values on the CPU this frame: no CPU-served binding reads
        // them and the host has not hinted the node (SetCpuAnalysisInterest).
        // Skipped nodes keep the values of their last readback. Off: every
        // dispatch reads back.
        bool skipUnneededCpuReadback{ false };

        // Queue a needed readback and collect it a frame or two later instead
        // of Mapping straight after the dispatch, which waits for the whole
        // GPU pipeline once per compute node. Takes effect only together with
        // skipUnneededCpuReadback.
        bool asyncAnalysisReadback{ false };

        // Minimum interval between readbacks a host hint asks for (the
        // selected node's panel, the canvas value labels). 0 reads hinted
        // nodes every frame. CPU-served bindings are never throttled.
        uint32_t cpuAnalysisHintThrottleMs{ 2000 };

        // Set D2D1_PROPERTY_CACHED on effect outputs so a clean subtree is not
        // re-executed when the terminal is drawn again. Costs one intermediate
        // per cached node. A kill switch for suspected stale frames.
        bool effectOutputCaching{ true };

        // Custom pixel shaders answer a tile query with the tile's own rect
        // for their image inputs (TRIVIAL_SAMPLING makes that exact). Off
        // answers with the whole image, which re-renders the upstream chain
        // once per tile once D2D tiles (past ~2048 px wide). A kill switch.
        bool subRectInputDemand{ true };

        // Record each compute dispatch on the runner's private deferred
        // context and submit it with one ExecuteCommandList. Off issues the
        // calls on the shared immediate context, where Direct2D on another
        // thread can interleave and a dispatch can silently write nothing:
        // a benchmark-only A/B mode.
        bool computeCommandList{ true };

        // Compile a build that a prebuilt effect's blob lacks (an effect
        // registered with EffectGraphRegisterEffect). Off falls back to a
        // build the blob has: the generic shader, with GPU-served bindings
        // read on the CPU instead.
        bool compilePrebuiltEffects{ false };

        bool operator==(const EvaluatorOptions&) const = default;
    };
}
