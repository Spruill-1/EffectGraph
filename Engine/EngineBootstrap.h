#pragma once

#include "pch_engine.h"

// Process-wide engine services a host starts and stops: the custom D2D
// effect classes and the shader bytecode cache.
namespace ShaderLab::Engine
{
    // Registers the engine's custom D2D effect classes. Returns the first
    // failure; every class is still attempted.
    HRESULT RegisterEngineD2DEffects(ID2D1Factory1* factory);

    // Configures the on-disk bytecode cache. An empty `rootPath` disables
    // persistence (tests, ad-hoc headless runs); otherwise the directory is
    // created if missing and entries untouched for `staleThresholdSec`
    // (default 90 days) are reaped as a job on the cache's worker queue.
    // 0 skips the reap. The engine picks no default root: the host owns the
    // path policy (the GUI uses %LOCALAPPDATA%\ShaderLab\bytecode).
    void ConfigureBytecodeCache(
        std::wstring rootPath,
        uint64_t staleThresholdSec = 90ull * 24 * 60 * 60);

    // Stops the engine's background threads: discards queued shader compiles
    // and cache maintenance, and joins the bytecode cache's workers. Hosts
    // call it once, after their last render, before the engine DLL unloads.
    // Idempotent; compiles requested afterwards run inline on the caller.
    void ShutdownEngineServices();
}
