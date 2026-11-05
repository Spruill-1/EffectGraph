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

        // Custom pixel shaders read their image pins with a point filter, so
        // a pixel renders the same in a clipped dirty-rect draw as in a whole
        // one (decision #242). Off: Direct2D's linear default, as in 1.0.
        // Separate from dirtyRegions so a forced-whole reference can render
        // with the same filter as the incremental draw it checks.
        bool pointImageFilter{ true };

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

        // Carry where a node changed, not only that it did: a source that
        // reports dirty rects (IEffectGraphInput2::InvalidateRects, a live
        // source's DirtyRects) dirties only the rects of what reads it per
        // pixel, and an incremental realize draws only those. Off: every
        // change is whole, and an incremental realize draws everything.
        bool dirtyRegions{ true };

        // Debug instrument for dirtyRegions: each incremental realize keeps
        // the target as it was, then checks that nothing outside the rects it
        // drew changed and that the target equals a whole render. Blocks on
        // two readbacks per realize.
        bool verifyDirtyRegions{ false };

        // Wake a compute node's bound consumers only when a field they read
        // changed. A dispatch over rects whose values reach the CPU in the
        // same frame is compared bit for bit with the values the consumers
        // last rendered with; any other dispatch dirties them all, and an
        // analysis-only node no longer dirties them before it has
        // dispatched. Off: every dispatch dirties every bound consumer.
        bool analysisChangeGate{ true };

        // Deferred analysis commit, for a host that cannot wait on the GPU
        // (EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT). Takes effect
        // only with skipUnneededCpuReadback. A compute node's dispatch over
        // rects copies its values to the CPU without waiting and leaves its
        // lane-3 texture as it was; when the copy lands (a later frame) each
        // field is compared with its committed value under its binding's
        // CommitPolicy, and only the fields that commit are written to the
        // texture and wake their consumers. A dispatch over the whole input
        // publishes at once, as before, except Hosted fields. Off: every
        // policy is Immediate.
        bool deferredAnalysisCommit{ false };

        // A statistics node (a SHADERLAB_REDUCE_CELLS shader) whose input
        // changed only in rects since its last dispatch recomputes only the
        // cells of its 8x8 grid the rects touch, and folds them with the
        // partials the other cells kept; the result equals a whole
        // dispatch's bit for bit. Off: every dispatch recomputes all 64 cells.
        bool incrementalReductions{ true };

        // An incremental realize applies the move rects it was handed
        // (IEffectGraphInput2::MoveRects) to its target as copies, where
        // every node from the input to the root carries them (a
        // shift-invariant pixel shader), instead of drawing their
        // destinations again. Off: every move is a dirty rect at its
        // destination; the realize still reports the moves.
        bool applyMoves{ true };

        bool operator==(const EvaluatorOptions&) const = default;
    };
}
