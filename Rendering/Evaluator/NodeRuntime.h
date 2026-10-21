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
    // compute consumer by ProcessDeferredCompute.
    struct PreRenderTarget
    {
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        UINT32 width{ 0 };
        UINT32 height{ 0 };
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

        void ResetEffect()
        {
            instance = {};
            variant = {};
            analysisTexture = {};
            asyncReadbackPending = false;
            cpuFieldsStale = false;
        }

        // Everything but the change counter.
        void ResetAll()
        {
            const uint64_t changes = outputChangeCount;
            *this = {};
            outputChangeCount = changes;
        }
    };
}
