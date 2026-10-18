#include "pch_engine.h"
#include "EngineBootstrap.h"
#include "../Effects/BytecodeCache.h"
#include "../Effects/CustomComputeBridgeEffect.h"
#include "../Effects/CustomComputeShaderEffect.h"
#include "../Effects/CustomPixelShaderEffect.h"

namespace ShaderLab::Engine
{
    HRESULT RegisterEngineD2DEffects(ID2D1Factory1* factory)
    {
        if (!factory) return E_INVALIDARG;
        HRESULT firstFailure = S_OK;
        for (HRESULT hr : { Effects::CustomPixelShaderEffect::RegisterEffect(factory),
                            Effects::CustomComputeShaderEffect::RegisterEffect(factory),
                            Effects::CustomComputeBridgeEffect::RegisterEffect(factory) })
        {
            if (FAILED(hr) && SUCCEEDED(firstFailure))
                firstFailure = hr;
        }
        return firstFailure;
    }

    void ConfigureBytecodeCache(std::wstring rootPath, uint64_t staleThresholdSec)
    {
        auto& cache = Effects::BytecodeCache::Instance();
        const bool reap = !rootPath.empty() && staleThresholdSec != 0;
        cache.SetDiskCacheRoot(std::move(rootPath));
        if (reap)
            cache.QueueReapDisk(staleThresholdSec);
    }

    void ShutdownEngineServices()
    {
        Effects::BytecodeCache::Instance().Shutdown();
    }
}
