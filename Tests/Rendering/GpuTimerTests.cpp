#include "pch_engine.h"
#include "../TestSupport.h"
#include "Rendering/GpuTimer.h"

#include <chrono>
#include <thread>

// GPU timing spans.
namespace
{
    using namespace ShaderLab::Tests;

    // A span closed without being opened in the same frame must not report:
    // its begin query would hold a timestamp from an earlier frame.
    SL_TEST_CASE(TestGpuTimerUnopenedSpan, Tag::Gpu)
    {
        using ShaderLab::Rendering::GpuSpan;
        ShaderLab::Rendering::GpuTimer timer;
        TEST("GpuTimer_Initializes", timer.Initialize(g_d3dDevice.get()));
        timer.SetEnabled(true);

        timer.BeginFrame();
        timer.Begin(GpuSpan::Evaluate);
        timer.End(GpuSpan::Evaluate);
        timer.EndFrame();
        timer.CollectBlocking();
        const double opened = timer.SpanMs(GpuSpan::Evaluate);

        // Cycle the ring back to the first slot, then close Evaluate unopened.
        for (uint32_t frame = 1; frame < ShaderLab::Rendering::GpuTimer::kLatency; ++frame)
        {
            timer.BeginFrame();
            timer.EndFrame();
            timer.CollectBlocking();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        timer.BeginFrame();
        timer.Begin(GpuSpan::Frame);
        timer.End(GpuSpan::Evaluate);
        timer.End(GpuSpan::Frame);
        timer.EndFrame();
        TEST("GpuTimer_FrameResolves", timer.CollectBlocking());
        TEST("GpuTimer_UnopenedSpanKeepsLastValue", timer.SpanMs(GpuSpan::Evaluate) == opened);
        timer.Shutdown();
    }
}
