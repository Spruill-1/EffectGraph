#pragma once

#include "../Rendering/EvaluatorOptions.h"
#include <atomic>

namespace ShaderLab::Performance
{
    // Process defaults for Rendering::EvaluatorOptions. An evaluator follows
    // these until GraphEvaluator::SetOptions pins its own, and reads them once
    // per frame. The GUI drives its evaluator through these setters; headless
    // pins options per evaluator from its command line. See EvaluatorOptions
    // for what each setting does.
    //
    // Written by UI and MCP threads, read by render threads. Every setting
    // uses acquire/release: relaxed would become visible just as soon, but
    // would not order the setting against the writes that preceded it. The
    // telemetry counters below are relaxed; they order nothing.
    Rendering::EvaluatorOptions DefaultEvaluatorOptions();
    void SetDefaultEvaluatorOptions(const Rendering::EvaluatorOptions& options);

    // Per-field shims onto the defaults above.
    bool IsGpuBindingsEnabled();
    void SetGpuBindingsEnabled(bool enabled);

    bool IsSkipUnneededCpuReadbackEnabled();
    void SetSkipUnneededCpuReadbackEnabled(bool enabled);

    bool IsAsyncAnalysisReadbackEnabled();
    void SetAsyncAnalysisReadbackEnabled(bool enabled);

    uint32_t CpuAnalysisHintThrottleMs();
    void SetCpuAnalysisHintThrottleMs(uint32_t ms);

    bool IsEffectOutputCachingEnabled();
    void SetEffectOutputCachingEnabled(bool enabled);

    bool IsSubRectInputDemandEnabled();
    void SetSubRectInputDemandEnabled(bool enabled);

    bool IsComputeCommandListEnabled();
    void SetComputeCommandListEnabled(bool enabled);

    // Process-wide telemetry, summed over every evaluator.
    // Dispatches whose CPU readback was skipped.
    uint64_t SkippedCpuReadbacks();
    void IncrementSkippedCpuReadbacks();
}
