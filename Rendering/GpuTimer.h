#pragma once

#include "pch_engine.h"

namespace ShaderLab::Rendering
{
    // GPU-side span timing via D3D11 timestamp queries.
    //
    // CPU wall-clock around D2D calls measures how long it took to RECORD
    // commands, not how long the GPU spent executing them, because D2D
    // submission is asynchronous. These spans measure the GPU side.
    //
    // THE D2D BATCHING TRAP. A timestamp is a command in the D3D11 stream, so
    // it orders correctly against other D3D11 commands -- but D2D buffers its
    // drawing internally and may not emit any GPU work until EndDraw. Closing a
    // span right after a D2D call therefore measures an empty stretch of the
    // command stream and reports ~0. Every End() overload that closes a span
    // around D2D work takes the device context and flushes it first; use those,
    // not the bare End(), or the span reads zero and looks like a fast shader.
    //
    // Results are collected kLatency frames late so GetData never blocks. A
    // span whose frame has not retired keeps its previous value, so callers can
    // read every frame without special-casing warm-up.
    enum class GpuSpan : uint32_t
    {
        Frame = 0,        // whole render: sources + evaluate + compute + draw
        SourcesPrep,      // PrepareSourceNode loop (image/video upload)
        Evaluate,         // GraphEvaluator::Evaluate passes
        DeferredCompute,  // ProcessDeferredCompute (D3D11 compute dispatches)
        Draw,             // DrawImage(preview) into the target
        Count
    };

    const wchar_t* GpuSpanName(GpuSpan s);

    class GpuTimer
    {
    public:
        static constexpr uint32_t kSpanCount = static_cast<uint32_t>(GpuSpan::Count);
        // Frames of query latency before results are collected. 4 is enough for
        // the deepest pipelining seen here and costs 4 * (1 + 2*kSpanCount)
        // queries, which is trivial.
        static constexpr uint32_t kLatency = 4;

        GpuTimer() = default;
        ~GpuTimer();
        GpuTimer(const GpuTimer&) = delete;
        GpuTimer& operator=(const GpuTimer&) = delete;

        // Creates the query pool. Safe to call twice; the second call is a
        // no-op unless the device changed.
        bool Initialize(ID3D11Device* device);
        void Shutdown();
        bool IsInitialized() const { return m_context != nullptr; }

        // Timing is OFF by default. It is not free: closing a span around D2D
        // work requires a Flush, which breaks D2D's batching and perturbs the
        // very frame being measured. Enable it to measure, not to run.
        void SetEnabled(bool on);
        bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }

        // Per-frame bracket. BeginFrame also collects any frame that has
        // retired, so results lag by up to kLatency frames.
        void BeginFrame();
        void EndFrame();

        void Begin(GpuSpan s);
        void End(GpuSpan s);
        // Preferred around D2D work -- flushes so the span cannot close before
        // the GPU commands it is meant to enclose have been emitted.
        void End(GpuSpan s, ID2D1DeviceContext* dcToFlush);

        // ---- per-node spans ------------------------------------------------
        //
        // The fixed spans above answer "where does the frame go"; these answer
        // "which node costs what". They are a separate pool because the node
        // set is dynamic and a graph can hold far more nodes than the handful
        // of phase spans.
        //
        // Direct2D makes per-node attribution awkward: it evaluates an effect
        // chain lazily at DrawImage, so image nodes are not separately
        // dispatched and cannot simply be bracketed in place. Compute nodes
        // CAN be -- each bridge dispatch is its own submission -- so those are
        // exact and free. Image nodes have to be materialised one at a time,
        // which is why that is a profiling mode rather than always on.
        static constexpr uint32_t kMaxNodeSpans = 64;

        void BeginNode(uint32_t nodeId);
        void EndNode(uint32_t nodeId);
        void EndNode(uint32_t nodeId, ID2D1DeviceContext* dcToFlush);

        // Milliseconds for a node from the most recently retired frame, or
        // -1.0 if that node was not measured.
        double NodeMs(uint32_t nodeId) const;
        const std::unordered_map<uint32_t, double>& NodeResults() const { return m_nodeResultMs; }
        void ClearNodeResults() { m_nodeResultMs.clear(); }

        // Benchmark helper: block until the frame just ended has retired, then
        // collect it. This deliberately stalls the CPU on the GPU -- exactly
        // what the ring buffer exists to avoid -- so it is for measurement
        // loops only, never the live render path. Without it a benchmark reads
        // SpanMs() before anything has retired and records the PREVIOUS frame's
        // number over and over, producing min == median == max and the false
        // impression of a very stable measurement.
        bool CollectBlocking(uint32_t timeoutMs = 2000);

        // Milliseconds of GPU time from the most recently retired frame.
        double SpanMs(GpuSpan s) const
        {
            auto i = static_cast<uint32_t>(s);
            return i < kSpanCount ? m_resultMs[i].load(std::memory_order_acquire) : 0.0;
        }
        bool     HasResults()     const { return FramesResolved() > 0; }
        // Diagnostic pair. framesOpened counts BeginFrame calls that actually
        // opened a frame; framesResolved counts ones whose queries came back.
        // opened==0 means the worker never ran with timing on; opened>>resolved
        // means the collect path is failing, which otherwise looks identical.
        uint64_t FramesOpened()   const { return m_framesOpened.load(std::memory_order_acquire); }
        uint64_t FramesResolved() const { return m_framesResolved.load(std::memory_order_acquire); }
        // Frames thrown away because the GPU clock was disjoint (a frequency
        // change mid-frame, e.g. DVFS). Non-zero here means the numbers moved
        // for a reason that has nothing to do with the shader.
        uint32_t DisjointDrops()  const { return m_disjointDrops.load(std::memory_order_acquire); }

    private:
        struct NodeSpan
        {
            uint32_t nodeId{ 0 };
            winrt::com_ptr<ID3D11Query> begin;
            winrt::com_ptr<ID3D11Query> end;
            bool used{ false };
        };

        struct FrameQueries
        {
            winrt::com_ptr<ID3D11Query> disjoint;
            winrt::com_ptr<ID3D11Query> begin[kSpanCount];
            winrt::com_ptr<ID3D11Query> end[kSpanCount];
            bool used[kSpanCount]{};
            // Begin issued this frame. End without it is ignored: the begin
            // query would hold a timestamp from kLatency frames ago, or none.
            bool begun[kSpanCount]{};
            bool inFlight{ false };
            // Grown on demand up to kMaxNodeSpans and then reused; the queries
            // are the expensive part, not the vector.
            std::vector<NodeSpan> nodeSpans;
        };

        // Index of this frame's span for `nodeId`, allocating one if there is
        // room. Returns -1 when the pool is full.
        int AcquireNodeSpan(FrameQueries& f, uint32_t nodeId);

        void Collect(FrameQueries& f);
        // Decodes a retired frame's spans into the results. False, keeping
        // the previous results, when its clock was disjoint.
        bool ResolveFrame(FrameQueries& f, const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& dj);

        winrt::com_ptr<ID3D11Device>        m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        FrameQueries m_frames[kLatency];
        uint32_t m_writeIndex{ 0 };
        // ATOMIC, deliberately: SetEnabled is called from the UI/MCP thread
        // while the render worker reads it every frame. As a plain bool on
        // ARM64's weak memory model the write was never observed by the
        // worker -- the route reported enabled=true while the render thread
        // kept sampling nothing, and framesResolved sat at 0.
        std::atomic<bool> m_enabled{ false };
        bool     m_frameOpen{ false };   // render thread only
        // ALL of these are written by the render worker and read from the
        // UI/MCP thread. As plain members on ARM64 the reader saw a value
        // frozen at its first observation -- framesOpened stuck at 1 while the
        // worker was happily counting, and every span reading 0.0, which is
        // indistinguishable from "the GPU did no work". Same failure the
        // m_enabled flag had in the other direction.
        std::atomic<double> m_resultMs[kSpanCount]{};
        std::atomic<uint64_t> m_framesOpened{ 0 };
        std::atomic<uint64_t> m_framesResolved{ 0 };
        std::atomic<uint32_t> m_disjointDrops{ 0 };

        // Per-node results from the last retired frame. Render thread only;
        // other threads see them through the published graph snapshot.
        std::unordered_map<uint32_t, double> m_nodeResultMs;
    };
}
