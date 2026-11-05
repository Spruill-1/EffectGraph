#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"
#include "EvaluatorOptions.h"

#include <memory>
#include <unordered_set>

namespace ShaderLab::Effects
{
    class IEffectHooks;
}

namespace ShaderLab::Rendering
{
    class GpuTimer;

    namespace Evaluator
    {
        class EvaluatorCore;
    }

    // Counters for incremental draws (IEffectGraphContext2::RealizeToIncremental),
    // cumulative; a host diffs them across a frame. Pixels count each rect
    // drawn, so overlapping rects count twice; a whole draw counts the target.
    struct RegionDrawStats
    {
        uint64_t realizes{ 0 };         // successful RealizeToIncremental calls, with or without the flag
        uint64_t draws{ 0 };            // of them, the ones that drew anything
        uint64_t wholeDraws{ 0 };       // of those, drawn whole (first, new target, Whole region, no flag, switch off)
        uint64_t rectsDrawn{ 0 };
        uint64_t pixelsDrawn{ 0 };      // RegionPixelsDrawn
        uint64_t pixelsTarget{ 0 };     // RegionPixelsTarget: the target's pixels, per realize (idle ones too)
        // verifyDirtyRegions: realizes checked, and pixels that failed each check.
        uint64_t verified{ 0 };
        uint64_t verifyOutsideMismatches{ 0 };   // changed outside the rects drawn
        uint64_t verifyWholeMismatches{ 0 };     // differ from a whole render
        // Moves (IEffectGraphInput2::MoveRects) reaching a realized root: applied
        // as copies (MovesApplied), drawn at their destinations instead
        // (MovesCollapsed: by a node on the way, the draw's plan, a whole draw,
        // or more than `moved` holds; counted only where `drawn` covers the
        // destination, so a dropped move is in neither), reported in `moved`, and the pixels the
        // applied ones copied (each copied twice, through a scratch texture).
        uint64_t movesApplied{ 0 };
        uint64_t movesCollapsed{ 0 };
        uint64_t movesReported{ 0 };
        uint64_t movePixelsCopied{ 0 };
        // verifyDirtyRegions: pixels inside applied moves' destinations that
        // differ from a whole render by no more than cMoveDitherCodes (an
        // 8-bit target): a dithering shift-invariant effect's expected drift,
        // not counted in verifyWholeMismatches.
        uint64_t verifyMoveDrift{ 0 };
    };

    // The code distance, per 8-bit channel, a moved region of a dithering
    // shift-invariant effect may differ from a fresh render by: dither of up
    // to one code either way at each of two positions, plus rounding. It
    // covers DitherQuantize up to strength 2 (noise support about ±0.62 ×
    // strength); an effect that dithers harder must not declare shiftInvariant.
    inline constexpr int cMoveDitherCodes = 3;

    // Compute analysis readbacks, by cause, and the builds that served a GPU
    // binding from the CPU; cumulative, per evaluator (one per context). A
    // host diffs them across a frame. A readback counts once, under the first
    // cause that applies in this order.
    struct AnalysisReadbackStats
    {
        uint64_t fallbackReadbacks{ 0 };    // a binding read on the CPU: a fallback build, a CPU-only parameter
        uint64_t freshReadbacks{ 0 };       // RequestFreshCpuReadback
        uint64_t unskippedReadbacks{ 0 };   // skipUnneededCpuReadback off
        uint64_t hintReadbacks{ 0 };        // SetCpuAnalysisInterest
        uint64_t skippedReadbacks{ 0 };     // dispatches that read nothing back
        // Non-blocking readbacks a deferred commit scheduled
        // (EvaluatorOptions::deferredAnalysisCommit): no cause above applied.
        uint64_t deferredReadbacks{ 0 };
        // Builds chosen without the GPU binding the plan wanted: the exact
        // build is missing (fallbackBuilds) or still compiling (pending).
        uint64_t fallbackBuilds{ 0 };
        uint64_t fallbackBuildsPending{ 0 };
    };

    // Needed nodes whose applied build dropped GPU routing their plan wanted,
    // now: the exact build is missing, or still compiling (pending).
    struct FallbackBuildsInUse
    {
        uint32_t missing{ 0 };
        uint32_t pending{ 0 };
    };

    // EvaluatorOptions::analysisChangeGate's verdicts, one per successful
    // dispatch of a compute node that has bound consumers; cumulative.
    struct BindingGateStats
    {
        uint64_t whole{ 0 };       // the input changed whole, or the node's own state did: all woken
        uint64_t unchanged{ 0 };   // over rects, with this frame's values: no bound field changed
        uint64_t changed{ 0 };     // over rects, with this frame's values: a bound field changed
        uint64_t noValue{ 0 };     // over rects, values not on the CPU this frame: all woken
        uint64_t deferred{ 0 };    // over rects, deferred commit: judged when the readback lands
    };

    // Cell reductions (SHADERLAB_REDUCE_CELLS, EvaluatorOptions::
    // incrementalReductions); cumulative, per evaluator. A host diffs them
    // across a frame.
    struct ReduceCellStats
    {
        uint64_t dispatches{ 0 };        // successful dispatches of a cell reduction
        uint64_t incremental{ 0 };       // of them, the ones that recomputed fewer than every cell
        uint64_t cellsDispatched{ 0 };   // ReduceCellsDispatched: cells recomputed
        uint64_t cellsTotal{ 0 };        // ReduceCellsTotal: the grid's cells, per dispatch
    };
    // One node's share of ReduceCellStats, and its last successful dispatch:
    // the cells it recomputed (bit c = cell c) and whether the evaluator
    // handed it rects (false: whole, whatever the runner then did).
    struct NodeReduceCellStats
    {
        ReduceCellStats totals;
        uint64_t lastMask{ 0 };
        bool lastRectsGiven{ false };
    };

    // Compute inputs the shader cannot read in place, drawn into a producer's
    // FP32 pre-render target; cumulative, per evaluator. With dirty regions
    // on, a target redraws only where its producer's output changed since it
    // was last drawn. Pixels count each rect drawn, as drawn: a pixel in two
    // overlapping rects is shaded, and counted, twice. A whole draw counts the
    // target.
    struct PreRenderStats
    {
        uint64_t draws{ 0 };          // targets drawn
        uint64_t wholeDraws{ 0 };     // of them, whole (first use, a new size or image, a Whole region, the switch off)
        uint64_t rectsDrawn{ 0 };
        uint64_t pixelsDrawn{ 0 };
        uint64_t pixelsTarget{ 0 };   // the target's pixels, per use (reuses too, a second consumer in one pass included)
        uint64_t reused{ 0 };         // uses that found the target current and drew nothing
        uint64_t failures{ 0 };       // draws or allocations that failed; the next use draws whole
    };

    // EvaluatorOptions::deferredAnalysisCommit: what landed and what it
    // committed; cumulative, per evaluator. A host diffs them across a frame.
    struct DeferredCommitStats
    {
        uint64_t wholeDispatches{ 0 };    // dispatches over the whole input or own state (stamped)
        uint64_t rectsDispatches{ 0 };    // dispatches over rects (judged on landing)
        uint64_t landed{ 0 };             // readbacks delivered
        uint64_t discarded{ 0 };          // delivered older than the first fill or the stamp
        uint64_t seeds{ 0 };              // records seeded
        uint64_t stamps{ 0 };             // a whole dispatch's own readback, recorded waking nobody
        uint64_t exactCommits{ 0 };       // fields committed under Exact
        uint64_t thresholdCommits{ 0 };   // fields committed under Threshold
        uint64_t subThreshold{ 0 };       // Threshold fields that moved within their threshold
        uint64_t hostedSeeds{ 0 };        // Hosted fields written by a seed that woke their consumers
        uint64_t hostedCommits{ 0 };      // fields committed by CommitFields
        uint64_t commitLandings{ 0 };     // landings that committed a field (engine commits)
        // Frames from the dispatch to the commit, over commitLandings.
        uint64_t landingFramesSum{ 0 };
        uint64_t landingFramesMax{ 0 };
        uint64_t blockingDrains{ 0 };     // PollReadbacks(block = true) that collected a readback
        uint64_t copyFailures{ 0 };       // copy passes that did not run; their landings write the texels
        uint64_t hostWritesKept{ 0 };     // fields a landing skipped: the host wrote them after its dispatch
        uint64_t readbackFailures{ 0 };   // slots whose Map failed for good; the producer redispatches whole
        // Frames from the dispatch whose landed values the host read to the
        // frame that applied its commit, per field committed by CommitFields.
        uint64_t hostedFramesSum{ 0 };
        uint64_t hostedFramesMax{ 0 };
    };

    // Evaluates an GraphModel by walking nodes in topological order,
    // creating / caching D2D effects, wiring inputs from upstream outputs,
    // and returning the final ID2D1Image* for presentation.
    //
    // The evaluator keeps a per-node effect cache so that effects are only
    // recreated when the node type or CLSID changes (not every frame).
    // Properties are re-applied each frame on dirty nodes.
    //
    // Hosts render frames through FrameExecutor, which orders the calls.
    //
    // The class holds only a pointer to its implementation
    // (Rendering/Evaluator/), so the exported layout does not change when
    // the evaluator's internals do.
    class GraphEvaluator
    {
    public:
        GraphEvaluator();
        ~GraphEvaluator();
        GraphEvaluator(const GraphEvaluator&) = delete;
        GraphEvaluator& operator=(const GraphEvaluator&) = delete;

        // Optional GPU timer, owned by the host. When set and enabled, the
        // evaluator brackets each compute dispatch so per-node GPU time can be
        // attributed exactly -- a compute dispatch is its own submission, so
        // this costs nothing and perturbs nothing. Image nodes are NOT timed
        // here: Direct2D evaluates an effect chain lazily at DrawImage, so
        // they are not separately dispatched and have to be materialised one
        // at a time, which is a profiling mode rather than free.
        void SetGpuTimer(GpuTimer* timer);

        // Pins this evaluator's options. Until then it follows the process
        // defaults (Performance::DefaultEvaluatorOptions). Callable from any
        // thread; the next Normal Evaluate picks the value up and holds it
        // for that frame.
        void SetOptions(const EvaluatorOptions& options);
        // Drops pinned options and follows the process defaults again.
        void FollowDefaultOptions();
        // The options the next frame will use.
        EvaluatorOptions Options() const;

        // Where built-in effects' C++ hooks (derived constants, image-output
        // sizing) come from. Defaults to the built-in catalog; nullptr runs
        // with no hooks. Not owned; must outlive the evaluator.
        void SetEffectHooks(const Effects::IEffectHooks* hooks);

        enum class EvalMode
        {
            Normal,
            // Re-applies properties and bindings only: queues no compute
            // dispatch, and leaves variant and async-readback polling to the
            // next Normal pass. FrameExecutor uses it for the sweep after
            // ProcessDeferredCompute.
            Frozen,
        };

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::GraphModel& graph, ID2D1DeviceContext5* dc,
                             EvalMode mode = EvalMode::Normal);

        // Run deferred D3D11 compute dispatches for statistics nodes.
        // Call AFTER Evaluate (and double-eval) when all D2D effects are
        // initialized, outside any draw session on `dc`: an input the shader
        // cannot read in place is rendered in a session of its own.
        // Returns true if any compute dispatches ran (analysis output changed).
        bool ProcessDeferredCompute(Graph::GraphModel& graph, ID2D1DeviceContext5* dc);
        // Compute dispatches Evaluate queued that ProcessDeferredCompute has
        // not run yet. ProcessDeferredCompute drains the queue, so this is 0
        // after a frame; DispatchesLastFrame() counts the work done.
        size_t DeferredComputeCount() const;

        // Compute dispatches actually issued during the last
        // ProcessDeferredCompute. Survives the drain.
        uint32_t DispatchesLastFrame() const;

        // Compile shaders on the cache's worker threads instead of the calling
        // thread (off by default). A node without its baseline shader has no
        // output, sets EffectNode::compilePending and stays dirty; a missing
        // variant falls back to the generic shader. The GUI turns it on so a
        // long compile does not stall its render thread; headless leaves it
        // off because a one-shot render must wait for the shader anyway.
        void SetAsyncCompile(bool on);

        // Async CPU readback (EvaluatorOptions::asyncAnalysisReadback, the
        // GUI's live loop): values copied for a node on one frame land on a
        // later Evaluate. True while any copy is still in flight, so a host
        // keeps rendering until the values have landed.
        bool HasPendingReadbacks() const;

        // Resolve property bindings for Source nodes only (lightweight, no D2D).
        // Call before TickAndUploadVideos so video nodes see updated Time values.
        void ResolveSourceBindings(Graph::GraphModel& graph);

        // Release all cached D2D effects (e.g., on device lost or graph clear).
        // The graph-aware overload also clears every node's non-owning cachedOutput
        // pointer so the render path can't dereference a freed effect.
        void ReleaseCache();
        void ReleaseCache(Graph::GraphModel& graph);

        // Drops everything kept for one node's effect instance (e.g., CLSID
        // changed). The graph-aware overload also clears node.cachedOutput.
        void InvalidateNode(uint32_t nodeId);
        void InvalidateNode(Graph::GraphModel& graph, uint32_t nodeId);

        // Update an existing cached effect's shader bytecode in-place (for recompile).
        // If the effect isn't cached yet, does nothing (next Evaluate will create it).
        void UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node);

        // The GUID a shader is loaded into D2D under: the definition's GUID
        // mixed with the bytecode's identity. D2D ignores LoadPixelShader for
        // a GUID it already holds, so different bytecode needs a different GUID.
        static GUID ShaderGuidFor(const GUID& definitionGuid, const std::vector<uint8_t>& bytecode);

        // Clean-subgraph caching telemetry (D2D1_PROPERTY_CACHED).
        size_t   CachedEffectCount() const;
        uint64_t CacheInvalidations() const;
        // Evaluate passes in which this node's output changed (it re-rendered,
        // or its analysis fields moved). A host compares it across a frame to
        // learn whether the node rendered in that frame.
        uint64_t OutputChangeCount(uint32_t nodeId) const;

        // Where the node's output changed since a host last acknowledged
        // drawing it (FrameRequest::acknowledgeRoots): the union of every
        // pass's region. Whole for a node never acknowledged. Read it in
        // FrameRequest::draw, after the frame's passes added to it.
        Graph::DirtyRegion PendingRegion(uint32_t nodeId) const;
        // Clears the pending region: the host drew all of it.
        void AcknowledgeRegion(uint32_t nodeId);
        // What incremental realizes drew, kept by the host that draws.
        const RegionDrawStats& RegionStats() const;
        RegionDrawStats& MutableRegionStats();
        // Nodes whose D2D output is not cached this frame: the roots an
        // incremental realize draws. A cached root reaches the target through an
        // intermediate, an uncached one is drawn into it directly, and the two
        // round differently; rects drawn now must match pixels drawn earlier.
        void SetUncachedNodes(std::unordered_set<uint32_t> ids);

        // Analysis readbacks by cause, and fallback builds.
        const AnalysisReadbackStats& ReadbackStats() const;
        // The analysis change gate's verdicts.
        const BindingGateStats& BindingGate() const;
        // What cell reductions recomputed.
        const ReduceCellStats& ReduceCells() const;
        NodeReduceCellStats ReduceCells(uint32_t nodeId) const;
        // What compute-input pre-renders drew.
        const PreRenderStats& PreRenders() const;
        // A producer's pre-render target's pending region (output pin 0 by
        // default): None when it is current, Whole when there is none.
        Graph::DirtyRegion PreRenderPending(uint32_t producerId, uint32_t outputPin = 0) const;
        // Needed nodes running a fallback build. Unlike the fallbackBuilds
        // counters, a clean node still running one counts on every frame.
        FallbackBuildsInUse FallbacksInUse(const Graph::GraphModel& graph) const;
        // The input region a D3D11 compute node's last successful dispatch
        // consumed: the rects its image inputs changed in since the dispatch
        // before, or Whole (its own state changed, its first dispatch). None
        // before its first dispatch.
        Graph::DirtyRegion DispatchInputRegion(uint32_t nodeId) const;
        // Test hook, this evaluator only: its next D3D11 compute dispatch fails
        // as a bridge failure would, without running. The dispatch it affects
        // clears it. Render thread only, like the rest of the evaluator.
        void FailNextComputeDispatchForTest(bool fail);
        bool FailNextComputeDispatchArmed() const;
        // Test hook, this evaluator only: its next pre-render draw of a compute
        // input fills the target with garbage and fails, as a failed EndDraw
        // after a partial draw would. The draw it affects clears it.
        void FailNextPreRenderDrawForTest(bool fail);
        bool FailNextPreRenderDrawArmed() const;
        // Test hook: the next lane-3 copy pass of D3D11 compute node `nodeId`
        // does not run, as when its resources cannot be created.
        void FailNextTextureCopyForTest(uint32_t nodeId);
        // Test hook: the node's next completed async readback fails its Map.
        void FailNextReadbackPollForTest(uint32_t nodeId);
        // Test hook: the node's next dispatch is recorded but its command
        // list fails to close, as FinishCommandList would, so none of it runs.
        void FailNextComputeSubmitForTest(uint32_t nodeId);

        // Host hint: nodes whose analysis values must reach the CPU (the
        // selected node, so its Properties panel stays live, plus any node an
        // MCP route is about to read). Consulted only when the skip-readback
        // option is on; with it off every compute dispatch reads back.
        //
        // Throttling: a node newly added to the set reads back at once, then
        // at most every EvaluatorOptions::cpuAnalysisHintThrottleMs. A node
        // that leaves the set and is re-added counts as new.
        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids);
        void ClearCpuAnalysisInterest();

        // Reads the node's analysis values back to the CPU, blocking, on its
        // next dispatch, whatever the skip, throttle and async settings say.
        // One-shot: the request is spent by that dispatch. A caller that needs
        // the values now also dirties the node and renders a frame. Only a
        // D3D11 compute node spends the request; one for another node stays
        // until ReleaseCache or the node leaves the graph. Call on the thread
        // that runs Evaluate.
        void RequestFreshCpuReadback(uint32_t nodeId);

        // GPU-binding bits of the shader build the node last ran: two bits per
        // gpuBindable parameter, 1 = structured buffer, 2 = analysis texture,
        // 0 = cbuffer. 0 for a node with no GPU-served binding.
        uint32_t AppliedGpuBits(uint32_t nodeId) const;

        // A D3D11 compute node's analysis results on the GPU, as the
        // StructuredBuffer<float4> its last dispatch wrote; null for any
        // other node or before its first dispatch.
        winrt::com_ptr<ID3D11ShaderResourceView> AnalysisBuffer(uint32_t nodeId) const;
        // Overwrites elements of that buffer until the node next dispatches,
        // refreshing the copy pixel-shader consumers read. Under a deferred
        // commit (`graph` given), only the texels of the fields written,
        // never a Hosted one's.
        HRESULT WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount,
                              const Graph::GraphModel* graph = nullptr);

        // ---- Deferred analysis commit (EvaluatorOptions::deferredAnalysisCommit)
        //
        // A compute producer's bound fields reach their consumers through a
        // committed record: the node's analysisOutput.fields, which its lane-3
        // texture matches texel for texel. Each dispatch copies its values
        // without waiting; when they land, each field is compared with its
        // committed value under its policy (PropertyBinding::commitPolicy) and
        // only changed fields are written, waking only their consumers.
        //
        // Collects the readbacks that have landed and commits them, as the
        // start of a Normal Evaluate does, drawing nothing. With `block`, waits
        // for each node's newest readback instead (counted in freshReadbacks).
        // True when any field was committed.
        bool PollReadbacks(Graph::GraphModel& graph, bool block = false);
        // The newest values read back (TryGetValues); false before any landed.
        bool LandedValues(uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values) const;
        // The committed record (GetCommittedValues); false before the seed.
        // `pending`: a bound field's texel holds a dispatch's values whose
        // readback has not landed, so consumers render with values the record
        // does not hold yet (its fields keep their last commit).
        bool CommittedValues(const Graph::GraphModel& graph, uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values,
                             bool& pending) const;
        // Records host commits of Hosted fields, applied at the start of the
        // next Normal Evaluate. E_NOT_VALID_STATE with the commit off;
        // E_INVALIDARG for a field that is not a bound, Hosted, scalar Float
        // field, or a non-finite value. Nothing is recorded on failure.
        HRESULT CommitFields(const Graph::GraphModel& graph, uint32_t nodeId, const uint32_t* fieldIndices,
                             const float* values, uint32_t count);
        // The policy of the producer's field, from the bindings that read it;
        // Immediate with the commit off, nullopt when bindings disagree.
        std::optional<Graph::CommitPolicy> FieldCommitPolicy(const Graph::GraphModel& graph, uint32_t nodeId,
                                                             uint32_t fieldIndex) const;
        const DeferredCommitStats& DeferredCommit() const;
        // Zeroes them, so a running maximum covers only what follows.
        void ResetDeferredCommitStats();
        // The texels of a D3D11 compute node's lane-3 texture, four floats
        // each, read back blocking: what pixel-shader consumers read. For
        // tests and diagnostics. E_NOT_VALID_STATE before its first fill.
        HRESULT ReadAnalysisTexture(uint32_t nodeId, std::vector<float>& float4s) const;

    private:
        std::unique_ptr<Evaluator::EvaluatorCore> m_core;
    };
}
