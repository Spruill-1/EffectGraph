#pragma once

#include "pch_engine.h"
#include "../../Effects/CustomPixelShaderEffect.h"
#include "../../Effects/CustomComputeShaderEffect.h"
#include "../../Effects/CustomComputeBridgeEffect.h"
#include "VariantSelector.h"

// Everything the evaluator keeps per node, in one record keyed by node id, so
// that invalidating a node is one reset and dropping it is one erase.
namespace ShaderLab::Rendering::Evaluator
{
    // The D2D effect a node renders with, and the impl behind it.
    struct EffectInstance
    {
        winrt::com_ptr<ID2D1Effect> effect;
        // Owning ref to the effect's output image. GetOutput hands back an
        // AddRef'd pointer; holding only EffectNode::cachedOutput (raw) lets
        // it die when D2D recreates its output proxy on an input toggle, and
        // the D2D debug layer flags the use-after-release.
        winrt::com_ptr<ID2D1Image> output;
        // Custom-effect impls, alive as long as `effect`.
        Effects::CustomPixelShaderEffect*   pixelImpl{ nullptr };
        Effects::CustomComputeShaderEffect* computeImpl{ nullptr };
        Effects::CustomComputeBridgeEffect* bridgeImpl{ nullptr };
        // D2D1_PROPERTY_CACHED as last set, so SetValue fires on transitions
        // only (a redundant SetValue may itself drop the cache).
        bool cacheEnabled{ false };

        bool HasCustomImpl() const { return pixelImpl || computeImpl; }
    };

    // A lane-3 producer's analysis texture wrapped as a D2D bitmap. A view of
    // the texture the copy pass writes, so it is rebuilt only when the
    // texture itself is replaced.
    struct AnalysisTextureView
    {
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        ID3D11Texture2D* texture{ nullptr };   // what `bitmap` wraps; compared, never dereferenced
    };

    // A persistent FP32 copy of one of this node's outputs, made for a
    // compute consumer by ProcessDeferredCompute. Shared by every compute
    // consumer of the output, any of which may not run on the frame the
    // output changes, so it keeps its own pending region.
    struct PreRenderTarget
    {
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        UINT32 width{ 0 };
        UINT32 height{ 0 };
        // The image pixel at texel (0, 0), and the image last drawn;
        // compared, never dereferenced.
        D2D1_POINT_2L origin{};
        const ID2D1Image* image{ nullptr };
        // Where the output changed since the bitmap was last drawn, in the
        // image's pixels (moves at their destinations): every pass's region
        // of the producer, unioned. Whole until the first draw and after a
        // failed one.
        Graph::DirtyRegion pending{ Graph::DirtyRegion::Whole() };
    };

    // A compute producer's deferred analysis commit (EvaluatorOptions::
    // deferredAnalysisCommit). The committed record is the node's
    // analysisOutput.fields, which bound consumers resolve from and which the
    // lane-3 texture matches texel for texel; this is the state around it.
    struct DeferredCommit
    {
        // The runner's buffers this state describes (BufferGeneration).
        uint64_t bufferGeneration{ 0 };
        // Dispatches, numbered from 1; a readback is known by the dispatch it copied.
        uint64_t dispatches{ 0 };
        // Every field holds a value read back since the texture's first fill.
        bool seeded{ false };
        // The first fill: the dispatch that copied every field into an empty
        // texture. A readback older than it is discarded.
        uint64_t firstFill{ 0 };
        // The stamp rule: the last dispatch over the whole input, which
        // copied every non-Hosted field to the texture at once. A readback
        // older than it is discarded; its own records the fields it left
        // unknown, waking nobody.
        uint64_t stamp{ 0 };
        // Per field: the record holds what the texture does (false after a
        // whole dispatch until its readback lands).
        std::vector<uint8_t> known;
        // Per field: committed by the host before the seed, which skips it,
        // and the value committed. The seed moves them into the record and
        // clears them.
        std::vector<uint8_t> hostOwned;
        std::vector<float> hostOwnedValue;
        // Per field: the dispatch count when the host last wrote its values
        // (SetFloats, WriteValues). A readback of that dispatch or an older
        // one does not describe the field any more; a later dispatch does.
        std::vector<uint64_t> hostWrite;
        // The newest values read back (TryGetValues) and their dispatch.
        std::vector<Graph::AnalysisFieldValue> landed;
        uint64_t landedDispatch{ 0 };
        // The frame that scheduled the landed values' dispatch.
        uint64_t landedFrame{ 0 };
        // Host commits waiting for the next Normal pass: (field, value).
        std::vector<std::pair<uint32_t, float>> hostCommits;
        // Async readbacks in flight: the runner's sequence number, the
        // dispatch it copied, the frame that scheduled it, and whether that
        // dispatch's copy pass failed (it wrote no texel).
        struct InFlight
        {
            uint64_t seq{ 0 }, dispatch{ 0 }, frame{ 0 };
            bool copyFailed{ false };
        };
        std::vector<InFlight> inFlight;
    };

    struct NodeRuntime
    {
        // The effect instance and what was derived from it. ResetEffect drops
        // these together: a lane-3 view kept past its bridge would serve the
        // old bridge's values to consumers.
        EffectInstance      instance;
        NodeVariantState    variant;
        AnalysisTextureView analysisTexture;
        bool                asyncReadbackPending{ false };
        // The last dispatch left its results on the GPU only: the CPU fields
        // are older than them, or empty.
        bool                cpuFieldsStale{ false };

        // Kept across ResetEffect.
        //
        // Pre-render targets of this node's outputs, by output pin. Keyed by
        // the producing node rather than the image pointer, so a recreated
        // effect reuses the full-size surface instead of leaking one.
        std::unordered_map<uint32_t, PreRenderTarget> preRenderTargets;
        // Last host-hint readback, for the throttle; unset reads back at once.
        std::optional<std::chrono::steady_clock::time_point> lastHintReadback;
        // RequestFreshCpuReadback: spent by the node's next compute dispatch.
        bool freshReadbackRequested{ false };
        // Evaluate passes in which the output changed. Never reset: hosts
        // compare it across a frame.
        uint64_t outputChangeCount{ 0 };
        // Where the output changed since a host last acknowledged drawing
        // it (FrameRequest::acknowledgeRoots): every pass's region, unioned.
        Graph::DirtyRegion pendingRegion{ Graph::DirtyRegion::Whole() };

        // D3D11 compute only. Its own state changed (a moved binding of its
        // own, a lane-1 source queued with it, a new effect), so its next
        // dispatch is whole whatever its inputs did. Cleared by a dispatch.
        bool ownStateChanged{ true };
        // The input region not yet consumed by a successful dispatch: what
        // the node's image inputs changed in since its last one, Whole for
        // its own state. Captured at queue time, since queueing clears the
        // node's dirty state.
        Graph::DirtyRegion undispatchedRegion;
        // What the last successful dispatch consumed.
        Graph::DirtyRegion lastDispatchInput;
        // Cell reductions this node dispatched (GraphEvaluator::ReduceCells(nodeId)):
        // cumulative counts, then the last successful dispatch's cells and
        // whether the evaluator handed it rects.
        uint64_t cellDispatches{ 0 }, cellIncremental{ 0 }, cellsDispatched{ 0 }, cellsTotal{ 0 };
        uint64_t lastCellMask{ 0 };
        bool lastCellRectsGiven{ false };
        // The node's CPU fields hold its last dispatch's values, which its
        // lane-3 texture and buffer also hold: the change gate's baseline.
        bool fieldsAreLastDispatch{ false };
        // Deferred analysis commit; reset with the effect, whose runner holds
        // the texture it describes.
        DeferredCommit deferred;

        void ResetEffect()
        {
            instance = {};
            variant = {};
            analysisTexture = {};
            asyncReadbackPending = false;
            cpuFieldsStale = false;
            ownStateChanged = true;
            fieldsAreLastDispatch = false;
            deferred = {};
            // The surfaces are kept; what a new effect renders is not known.
            for (auto& [pin, target] : preRenderTargets)
                target.pending = Graph::DirtyRegion::Whole();
        }

        // Everything but the change counter; the pending region becomes whole.
        void ResetAll()
        {
            const uint64_t changes = outputChangeCount;
            *this = {};
            outputChangeCount = changes;
        }
    };
}
