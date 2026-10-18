#include "pch_engine.h"
#include "Performance.h"

namespace ShaderLab::Performance
{
    namespace
    {
        constexpr Rendering::EvaluatorOptions scBuiltInDefaults{};

        std::atomic<bool>     s_gpuBindings{ scBuiltInDefaults.gpuBindings };
        std::atomic<bool>     s_skipReadback{ scBuiltInDefaults.skipUnneededCpuReadback };
        std::atomic<bool>     s_asyncReadback{ scBuiltInDefaults.asyncAnalysisReadback };
        std::atomic<uint32_t> s_hintThrottleMs{ scBuiltInDefaults.cpuAnalysisHintThrottleMs };
        std::atomic<bool>     s_outputCaching{ scBuiltInDefaults.effectOutputCaching };
        std::atomic<bool>     s_subRectInputDemand{ scBuiltInDefaults.subRectInputDemand };
        std::atomic<bool>     s_computeCommandList{ scBuiltInDefaults.computeCommandList };

        std::atomic<uint64_t> s_skippedReadbacks{ 0 };

        template <typename T>
        T Load(const std::atomic<T>& flag) { return flag.load(std::memory_order_acquire); }
        template <typename T>
        void Store(std::atomic<T>& flag, T value) { flag.store(value, std::memory_order_release); }
    }

    Rendering::EvaluatorOptions DefaultEvaluatorOptions()
    {
        Rendering::EvaluatorOptions options;
        options.gpuBindings               = Load(s_gpuBindings);
        options.skipUnneededCpuReadback   = Load(s_skipReadback);
        options.asyncAnalysisReadback     = Load(s_asyncReadback);
        options.cpuAnalysisHintThrottleMs = Load(s_hintThrottleMs);
        options.effectOutputCaching       = Load(s_outputCaching);
        options.subRectInputDemand        = Load(s_subRectInputDemand);
        options.computeCommandList        = Load(s_computeCommandList);
        return options;
    }

    void SetDefaultEvaluatorOptions(const Rendering::EvaluatorOptions& options)
    {
        Store(s_gpuBindings,        options.gpuBindings);
        Store(s_skipReadback,       options.skipUnneededCpuReadback);
        Store(s_asyncReadback,      options.asyncAnalysisReadback);
        Store(s_hintThrottleMs,     options.cpuAnalysisHintThrottleMs);
        Store(s_outputCaching,      options.effectOutputCaching);
        Store(s_subRectInputDemand, options.subRectInputDemand);
        Store(s_computeCommandList, options.computeCommandList);
    }

    bool IsGpuBindingsEnabled()                   { return Load(s_gpuBindings); }
    void SetGpuBindingsEnabled(bool enabled)      { Store(s_gpuBindings, enabled); }

    bool IsSkipUnneededCpuReadbackEnabled()              { return Load(s_skipReadback); }
    void SetSkipUnneededCpuReadbackEnabled(bool enabled) { Store(s_skipReadback, enabled); }

    bool IsAsyncAnalysisReadbackEnabled()              { return Load(s_asyncReadback); }
    void SetAsyncAnalysisReadbackEnabled(bool enabled) { Store(s_asyncReadback, enabled); }

    uint32_t CpuAnalysisHintThrottleMs()          { return Load(s_hintThrottleMs); }
    void SetCpuAnalysisHintThrottleMs(uint32_t ms) { Store(s_hintThrottleMs, ms); }

    bool IsEffectOutputCachingEnabled()              { return Load(s_outputCaching); }
    void SetEffectOutputCachingEnabled(bool enabled) { Store(s_outputCaching, enabled); }

    bool IsSubRectInputDemandEnabled()              { return Load(s_subRectInputDemand); }
    void SetSubRectInputDemandEnabled(bool enabled) { Store(s_subRectInputDemand, enabled); }

    bool IsComputeCommandListEnabled()              { return Load(s_computeCommandList); }
    void SetComputeCommandListEnabled(bool enabled) { Store(s_computeCommandList, enabled); }

    uint64_t SkippedCpuReadbacks()     { return s_skippedReadbacks.load(std::memory_order_relaxed); }
    void IncrementSkippedCpuReadbacks() { s_skippedReadbacks.fetch_add(1, std::memory_order_relaxed); }
}
