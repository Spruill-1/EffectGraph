#include "pch_engine.h"
#include "GpuTimer.h"

namespace ShaderLab::Rendering
{
    const wchar_t* GpuSpanName(GpuSpan s)
    {
        switch (s)
        {
        case GpuSpan::Frame:           return L"frame";
        case GpuSpan::SourcesPrep:     return L"sourcesPrep";
        case GpuSpan::Evaluate:        return L"evaluate";
        case GpuSpan::DeferredCompute: return L"deferredCompute";
        case GpuSpan::Draw:            return L"draw";
        default:                       return L"?";
        }
    }

    GpuTimer::~GpuTimer() { Shutdown(); }

    bool GpuTimer::Initialize(ID3D11Device* device)
    {
        if (!device) return false;
        if (m_device.get() == device && m_context) return true;
        Shutdown();

        m_device.copy_from(device);
        m_device->GetImmediateContext(m_context.put());
        if (!m_context) { m_device = nullptr; return false; }

        D3D11_QUERY_DESC dj{};  dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC ts{};  ts.Query = D3D11_QUERY_TIMESTAMP;

        for (auto& f : m_frames)
        {
            if (FAILED(m_device->CreateQuery(&dj, f.disjoint.put())))
            {
                // Timestamp queries are optional on some drivers/feature
                // levels. Fail cleanly rather than half-initialized: every
                // caller checks IsInitialized() and simply reports no GPU
                // timings, which is honest, instead of reporting zeros that
                // read as "the GPU did no work".
                Shutdown();
                return false;
            }
            for (uint32_t i = 0; i < kSpanCount; ++i)
            {
                if (FAILED(m_device->CreateQuery(&ts, f.begin[i].put())) ||
                    FAILED(m_device->CreateQuery(&ts, f.end[i].put())))
                {
                    Shutdown();
                    return false;
                }
            }
        }
        return true;
    }

    void GpuTimer::Shutdown()
    {
        for (auto& f : m_frames)
        {
            f.disjoint = nullptr;
            for (uint32_t i = 0; i < kSpanCount; ++i)
            {
                f.begin[i] = nullptr;
                f.end[i] = nullptr;
                f.used[i] = false;
            }
            f.inFlight = false;
            f.nodeSpans.clear();
        }
        m_nodeResultMs.clear();
        m_context = nullptr;
        m_device = nullptr;
        m_frameOpen = false;
        m_writeIndex = 0;
    }

    void GpuTimer::SetEnabled(bool on)
    {
        // Any thread. The frame state belongs to the render thread, which
        // notices the change at its next BeginFrame.
        m_enabled.store(on, std::memory_order_release);
    }

    void GpuTimer::BeginFrame()
    {
        if (!m_context) return;
        if (!m_enabled.load(std::memory_order_acquire))
        {
            // Abandon anything in flight. Results keep their last values so a
            // reader that polls across the toggle sees stale-but-labelled
            // numbers rather than a sudden run of zeros.
            for (auto& f : m_frames) f.inFlight = false;
            return;
        }

        // Collect the oldest slot before reusing it. At kLatency frames of
        // distance its data is normally ready, so this does not stall; if it
        // is not ready the slot is simply dropped.
        FrameQueries& f = m_frames[m_writeIndex];
        if (f.inFlight) Collect(f);

        for (uint32_t i = 0; i < kSpanCount; ++i) f.used[i] = f.begun[i] = false;
        // Keep the allocated queries, drop only the used flags: a node that
        // goes unmeasured this frame must not report last frame's number.
        for (auto& ns : f.nodeSpans) ns.used = false;
        m_context->Begin(f.disjoint.get());
        f.inFlight = true;
        m_frameOpen = true;
        m_framesOpened.fetch_add(1, std::memory_order_release);
    }

    void GpuTimer::Begin(GpuSpan s)
    {
        if (!m_enabled || !m_frameOpen) return;
        auto i = static_cast<uint32_t>(s);
        if (i >= kSpanCount) return;
        FrameQueries& f = m_frames[m_writeIndex];
        // A timestamp query is ended, never begun -- End() is what samples the
        // clock. Calling Begin() on one is a no-op that some debug layers flag.
        m_context->End(f.begin[i].get());
        f.begun[i] = true;
    }

    void GpuTimer::End(GpuSpan s)
    {
        if (!m_enabled || !m_frameOpen) return;
        auto i = static_cast<uint32_t>(s);
        if (i >= kSpanCount) return;
        FrameQueries& f = m_frames[m_writeIndex];
        if (!f.begun[i]) return;
        m_context->End(f.end[i].get());
        f.used[i] = true;
    }

    void GpuTimer::End(GpuSpan s, ID2D1DeviceContext* dcToFlush)
    {
        // See the header: without this flush the span can close before D2D has
        // emitted the work it is supposed to enclose, and reports ~0.
        if (m_enabled && m_frameOpen && dcToFlush) dcToFlush->Flush();
        End(s);
    }

    void GpuTimer::EndFrame()
    {
        // Close an open frame even if timing was switched off mid-frame, so
        // its disjoint query is never left begun.
        if (!m_frameOpen || !m_context) return;
        FrameQueries& f = m_frames[m_writeIndex];
        m_context->End(f.disjoint.get());
        m_frameOpen = false;
        m_writeIndex = (m_writeIndex + 1) % kLatency;
    }

    int GpuTimer::AcquireNodeSpan(FrameQueries& f, uint32_t nodeId)
    {
        for (size_t i = 0; i < f.nodeSpans.size(); ++i)
            if (f.nodeSpans[i].nodeId == nodeId) return static_cast<int>(i);
        if (f.nodeSpans.size() >= kMaxNodeSpans) return -1;

        NodeSpan ns;
        ns.nodeId = nodeId;
        D3D11_QUERY_DESC ts{};
        ts.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(m_device->CreateQuery(&ts, ns.begin.put())) ||
            FAILED(m_device->CreateQuery(&ts, ns.end.put())))
            return -1;
        f.nodeSpans.push_back(std::move(ns));
        return static_cast<int>(f.nodeSpans.size() - 1);
    }

    void GpuTimer::BeginNode(uint32_t nodeId)
    {
        if (!m_enabled || !m_frameOpen) return;
        FrameQueries& f = m_frames[m_writeIndex];
        int i = AcquireNodeSpan(f, nodeId);
        if (i < 0) return;
        m_context->End(f.nodeSpans[i].begin.get());
    }

    void GpuTimer::EndNode(uint32_t nodeId)
    {
        if (!m_enabled || !m_frameOpen) return;
        FrameQueries& f = m_frames[m_writeIndex];
        int i = AcquireNodeSpan(f, nodeId);
        if (i < 0) return;
        m_context->End(f.nodeSpans[i].end.get());
        f.nodeSpans[i].used = true;
    }

    void GpuTimer::EndNode(uint32_t nodeId, ID2D1DeviceContext* dcToFlush)
    {
        // Same reason as the phase-span overload: without the flush the span
        // can close before D2D has emitted the work it is meant to enclose.
        if (m_enabled && m_frameOpen && dcToFlush) dcToFlush->Flush();
        EndNode(nodeId);
    }

    double GpuTimer::NodeMs(uint32_t nodeId) const
    {
        auto it = m_nodeResultMs.find(nodeId);
        return it == m_nodeResultMs.end() ? -1.0 : it->second;
    }

    bool GpuTimer::CollectBlocking(uint32_t timeoutMs)
    {
        if (!m_enabled || !m_context) return false;
        // The frame most recently closed by EndFrame().
        uint32_t idx = (m_writeIndex + kLatency - 1) % kLatency;
        FrameQueries& f = m_frames[idx];
        if (!f.inFlight) return false;

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        for (;;)
        {
            // No DONOTFLUSH here: we WANT the flush, otherwise the commands
            // may sit unsubmitted and this spins until the timeout.
            HRESULT hr = m_context->GetData(f.disjoint.get(), &dj, sizeof(dj), 0);
            if (hr == S_OK) break;
            if (hr != S_FALSE) { f.inFlight = false; return false; }
            if (std::chrono::steady_clock::now() > deadline) { f.inFlight = false; return false; }
            std::this_thread::yield();
        }
        f.inFlight = false;
        return ResolveFrame(f, dj);
    }

    void GpuTimer::Collect(FrameQueries& f)
    {
        f.inFlight = false;

        // NOTE: no DONOTFLUSH. With it the ring never retired at all under
        // vsync -- GetData kept returning S_FALSE because the commands sat
        // unsubmitted, so every slot was dropped on reuse and framesResolved
        // stayed at 0 forever, which reads exactly like "the GPU did no work".
        // One flush per frame at the point we are about to reuse the slot is
        // cheap and guarantees forward progress.
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        HRESULT hr = m_context->GetData(f.disjoint.get(), &dj, sizeof(dj), 0);
        if (hr != S_OK) return;          // still not retired -- drop, keep old values
        ResolveFrame(f, dj);
    }

    bool GpuTimer::ResolveFrame(FrameQueries& f, const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& dj)
    {
        if (dj.Disjoint || dj.Frequency == 0)
        {
            // The GPU clock changed frequency mid-frame (DVFS, power state).
            // These samples are meaningless; counting them lets a caller see
            // that a surprising number came from clock instability.
            m_disjointDrops.fetch_add(1, std::memory_order_release);
            return false;
        }

        const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
        for (uint32_t i = 0; i < kSpanCount; ++i)
        {
            if (!f.used[i]) continue;
            UINT64 t0 = 0, t1 = 0;
            if (m_context->GetData(f.begin[i].get(), &t0, sizeof(t0), 0) != S_OK) continue;
            if (m_context->GetData(f.end[i].get(), &t1, sizeof(t1), 0) != S_OK) continue;
            if (t1 < t0) continue;       // wrapped or reordered; ignore
            m_resultMs[i].store(static_cast<double>(t1 - t0) * toMs, std::memory_order_release);
        }
        // Per-node spans. Rebuilt from scratch on each retire, so a node that
        // stopped being measured disappears instead of going stale.
        m_nodeResultMs.clear();
        for (const auto& ns : f.nodeSpans)
        {
            if (!ns.used) continue;
            UINT64 a = 0, b = 0;
            if (m_context->GetData(ns.begin.get(), &a, sizeof(a), 0) != S_OK) continue;
            if (m_context->GetData(ns.end.get(), &b, sizeof(b), 0) != S_OK) continue;
            if (b < a) continue;
            m_nodeResultMs[ns.nodeId] = static_cast<double>(b - a) * toMs;
        }
        m_framesResolved.fetch_add(1, std::memory_order_release);
        return true;
    }
}
