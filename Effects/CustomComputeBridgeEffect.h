#pragma once

#include "pch_engine.h"
#include "../Graph/PropertyValue.h"
#include "IEngineComputeOutput.h"
#include "IEngineComputeTexture.h"
#include "../Rendering/D3D11ComputeRunner.h"

namespace ShaderLab::Effects
{
    // Integer pixel rect a compute input occupies, used to size and place
    // its FP32 pre-render. One helper for every pre-render site, because the
    // two ways of getting this wrong were both live:
    //   * UNBOUNDED inputs (Flood, Tile, Border, Turbulence) report infinite
    //     local bounds. `right - left` overflowed to +inf, min() turned it
    //     into 8192, and the pre-render allocated 8192x8192 FP32 -- 1 GiB --
    //     drawn at an offset of about +FLT_MAX. Now E_BOUNDS, which callers
    //     surface as a runtime error telling the user to crop first.
    //   * FRACTIONAL bounds (a Scale or 2D affine upstream) were truncated,
    //     dropping the last partial row/column, and drawn at a sub-pixel
    //     offset, so the compute read bilinearly resampled pixels rather
    //     than the image. Snapping outward to whole pixels and drawing at
    //     the integer origin fixes both.
    // An extent past the D3D11 texture limit (16384) cannot be bound to a
    // compute shader and returns D2DERR_MAX_TEXTURE_SIZE_EXCEEDED.
    inline HRESULT SnapComputeInputRect(const D2D1_RECT_F& b, D2D1_RECT_L& out)
    {
        constexpr float kHuge = 1.0e7f;
        const float c[4] = { b.left, b.top, b.right, b.bottom };
        for (float v : c)
            if (!std::isfinite(v) || std::fabs(v) > kHuge) return E_BOUNDS;
        LONG L = static_cast<LONG>(std::floor(b.left));
        LONG T = static_cast<LONG>(std::floor(b.top));
        LONG R = static_cast<LONG>(std::ceil(b.right));
        LONG B = static_cast<LONG>(std::ceil(b.bottom));
        if (R <= L || B <= T) return E_NOT_VALID_STATE;
        constexpr LONG cMaxExtent = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        if (R - L > cMaxExtent || B - T > cMaxExtent) return D2DERR_MAX_TEXTURE_SIZE_EXCEEDED;
        out = D2D1_RECT_L{ L, T, R, B };
        return S_OK;
    }

    // CustomComputeBridgeEffect
    // =========================
    //
    // A regular D2D effect (`ID2D1EffectImpl` + `ID2D1DrawTransform`)
    // that wraps user-authored D3D11 compute shaders. From the
    // graph's perspective it behaves like any other effect: lives in
    // `GraphModel::EffectNode::cachedEffect`, takes one image input,
    // and produces an image output. Internally it dispatches a D3D11
    // compute shader against the input each time the host calls
    // `Dispatch`, populating either:
    //
    //   * an analysis structured buffer (for data-only nodes such as
    //     Luminance Statistics / Channel Statistics / Chromaticity
    //     Statistics / gamut-coverage analysis), readable by
    //     downstream consumers via `IEngineComputeOutput::GetAnalysisSrv`
    //     (GPU bindings, lane 1), or
    //
    //   * a D3D11-compute-produced output texture wrapped as an
    //     `ID2D1Bitmap1` (for image-producing nodes such as
    //     CIE Histogram), exposed via `GetImageOutput` for the
    //     evaluator to install as `node->cachedOutput`.
    //
    // The effect's own D2D output (its pass-through pixel shader) is
    // unused -- downstream nodes wire to `node->cachedOutput`
    // directly. The pass-through exists only so the effect satisfies
    // D2D's effect-shape contract (one input -> one output). The
    // evaluator creates it like any other effect and drives `Dispatch`
    // from ProcessDeferredCompute.

    // {149A1DAA-1CF1-4298-862C-5A7F3C1B4CD1}
    inline constexpr GUID IID_ICustomComputeBridge = {
        0x149a1daa, 0x1cf1, 0x4298,
        { 0x86, 0x2c, 0x5a, 0x7f, 0x3c, 0x1b, 0x4c, 0xd1 }
    };

    // Engine-internal interface the host uses to drive bridge
    // dispatches. Independent of `IEngineComputeOutput`, which is the
    // *consumer*-side interface; this one is the *driver* side.
    struct __declspec(uuid("149A1DAA-1CF1-4298-862C-5A7F3C1B4CD1"))
    ICustomComputeBridge : IUnknown
    {
        // Install the user's compiled compute shader bytecode. Called
        // once per (effect, definition-version). The bridge compiles
        // the D3D11 shader on the first dispatch.
        virtual HRESULT STDMETHODCALLTYPE SetCompiledBytecode(
            const BYTE* bytecode, UINT32 bytecodeSize) = 0;

        // Dispatch the user compute shader against `inputImage`.
        //
        // - `inputImage` is the upstream D2D image (typically
        //   `srcNode->cachedOutput`). The bridge pre-renders it into
        //   an FP32 bitmap, hands the texture to the D3D11 compute
        //   runner, and flushes D2D before dispatch.
        // - `cbufferData` is the user-property cbuffer bytes; the
        //   runner prepends `Width` + `Height` automatically.
        // - `analysisFloat4Count` is the number of float4 slots the
        //   shader writes to the analysis structured buffer
        //   (`RWStructuredBuffer<float4> Result : register(u0)`).
        //   Use 0 if the shader has no analysis output.
        // - `imageOutputW` / `imageOutputH` request a D3D11-compute-
        //   produced output texture of that size. Pass 0 / 0 for
        //   analysis-only effects.
        // - `outAnalysisFloats` (optional, may be null) collects a
        //   CPU-side copy of the analysis output for hosts that need
        //   to populate `EffectNode::analysisOutput.fields`.
        //
        // After a successful return the analysis SRV is available via
        // `IEngineComputeOutput::GetAnalysisSrv`, and the image
        // output (when requested) via `GetImageOutput`.
        virtual HRESULT STDMETHODCALLTYPE Dispatch(
            ID2D1DeviceContext5* dc,
            ID2D1Image* const* inputImages,
            UINT32 inputCount,
            const BYTE* cbufferData,
            UINT32 cbufferSize,
            UINT32 analysisFloat4Count,
            UINT32 imageOutputW,
            UINT32 imageOutputH,
            std::vector<float>* outAnalysisFloats) = 0;

        // The bitmap returned by the most recent `Dispatch` call when
        // it requested an image output. Lifetime is tied to the
        // bridge effect's lifetime; downstream nodes can treat it as
        // a non-owning `ID2D1Bitmap1*`. Returns nullptr for analysis-
        // only effects.
        virtual ID2D1Bitmap1* STDMETHODCALLTYPE GetImageOutput() = 0;

        // GPU bindings, consumer side. The evaluator calls
        // this for each binding it routes upstream-effect-SRV ->
        // consumer-t-slot before each dispatch. Bindings are cleared
        // automatically at the end of every Dispatch so a stale
        // binding doesn't leak into the next frame's dispatch.
        // Slot is the HLSL `t<slot>` register (1, 2, ...). srv is
        // the upstream effect's IEngineComputeOutput::GetAnalysisSrv()
        // result (non-owning -- caller guarantees lifetime through the
        // dispatch call).
        virtual HRESULT STDMETHODCALLTYPE SetGpuBinding(
            UINT32 slot, ID3D11ShaderResourceView* srv) = 0;

        // Set the dispatch dimensions for the next call. Defaults to
        // (1,1,1) for analysis-only effects; image-producing per-pixel
        // effects call this before Dispatch with (W/tx, H/ty, 1) where tx,ty are the
        // shader's [numthreads] dimensions.
        virtual HRESULT STDMETHODCALLTYPE SetDispatchDims(
            UINT32 x, UINT32 y, UINT32 z) = 0;
    };

    // The actual D2D effect class. Implements:
    //   * `ID2D1EffectImpl` + `ID2D1DrawTransform` -- the pass-through
    //     pixel shader that satisfies D2D's "one input -> one output"
    //     contract. Output is unused (downstream nodes wire to
    //     `node->cachedOutput`).
    //   * `ICustomComputeBridge` -- driver-side interface for the
    //     evaluator's deferred-compute pass.
    //   * `IEngineComputeOutput` / `IEngineComputeTexture` -- the
    //     consumer-side GPU-binding lanes 1 and 3. Delegate to the runner.
    class CustomComputeBridgeEffect
        : public ID2D1EffectImpl
        , public ID2D1DrawTransform
        , public ICustomComputeBridge
        , public IEngineComputeOutput
        , public IEngineComputeTexture
    {
    public:
        // {6D69E5C2-1AC0-481E-9F94-3DB8CCAD5710}
        static constexpr GUID CLSID_CustomComputeBridge =
        { 0x6d69e5c2, 0x1ac0, 0x481e, { 0x9f, 0x94, 0x3d, 0xb8, 0xcc, 0xad, 0x57, 0x10 } };

        // Register this effect with a D2D factory. Call once at startup.
        // Single shared CLSID across all D3D11 compute custom-effect
        // instances -- the per-instance shader bytecode is set via
        // `SetCompiledBytecode` after `CreateEffect`.
        static HRESULT RegisterEffect(ID2D1Factory1* factory);

        // Creates a bridge effect and returns its impl too, so the evaluator
        // can drive the dispatch. The impl lives as long as outEffect.
        static HRESULT Create(ID2D1DeviceContext* dc,
            winrt::com_ptr<ID2D1Effect>& outEffect, CustomComputeBridgeEffect** outImpl);

        // D2D creation callback (invoked by CreateEffect).
        static HRESULT __stdcall CreateFactory(IUnknown** effect);

        // ---- IUnknown ----
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(
            ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph* transformGraph) override;

        // ---- ID2D1DrawTransform ----
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override;

        // ---- ID2D1Transform ----
        IFACEMETHODIMP MapInputRectsToOutputRect(
            const D2D1_RECT_L* inputRects,
            const D2D1_RECT_L* inputOpaqueSubRects,
            UINT32 inputRectCount,
            D2D1_RECT_L* outputRect,
            D2D1_RECT_L* outputOpaqueSubRect) override;
        IFACEMETHODIMP MapOutputRectToInputRects(
            const D2D1_RECT_L* outputRect,
            D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const override;
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex,
            D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;

        // ---- ID2D1TransformNode ----
        IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 1; }

        // ---- ICustomComputeBridge ----
        HRESULT STDMETHODCALLTYPE SetCompiledBytecode(
            const BYTE* bytecode, UINT32 bytecodeSize) override;
        HRESULT STDMETHODCALLTYPE Dispatch(
            ID2D1DeviceContext5* dc,
            ID2D1Image* const* inputImages,
            UINT32 inputCount,
            const BYTE* cbufferData,
            UINT32 cbufferSize,
            UINT32 analysisFloat4Count,
            UINT32 imageOutputW,
            UINT32 imageOutputH,
            std::vector<float>* outAnalysisFloats) override;
        ID2D1Bitmap1* STDMETHODCALLTYPE GetImageOutput() override
        { return m_imageOutput.get(); }

        HRESULT STDMETHODCALLTYPE SetGpuBinding(
            UINT32 slot, ID3D11ShaderResourceView* srv) override;
        HRESULT STDMETHODCALLTYPE SetDispatchDims(
            UINT32 x, UINT32 y, UINT32 z) override;

        // Async CPU readback for the NEXT Dispatch only: the copy is queued
        // and the values are collected later through PollAnalysisReadback,
        // instead of Map() draining the whole GPU pipeline right after the
        // submit. Reset after each Dispatch.
        void SetAsyncReadback(bool async) { m_asyncReadback = async; }
        // EvaluatorOptions::computeCommandList for this bridge's dispatches.
        void SetComputeCommandList(bool on) { m_runner.UseImmediateContextForBenchmark(!on); }
        // Newest completed async readback, if any landed since the last call.
        bool PollAnalysisReadback(std::vector<float>& out);
        bool HasPendingReadback() const { return m_runner.HasPendingReadback(); }
        HRESULT WriteResults(uint32_t firstElement, const float* float4s, uint32_t elementCount)
        {
            return m_runner.WriteResults(firstElement, float4s, elementCount);
        }
        // Blocking read of the last dispatch's results.
        HRESULT ReadResults(std::vector<float>& out) { return m_runner.ReadResults(out); }
        // The last blocking readback's Map result (the dispatch itself succeeded).
        HRESULT LastReadbackResult() const { return m_runner.LastReadbackResult(); }

        // The step the last Dispatch failed at, None after a success.
        enum class DispatchStage
        {
            None,
            Arguments,      // a null context or input
            Device,         // no D3D11 device behind the context
            ShaderCreate,   // CreateComputeShader refused the bytecode
            NoShader,       // no bytecode installed yet
            InputBounds,    // an input is unbounded, empty or too large
            InputCopy,      // copying an input to an FP32 texture
            ImageOutput,    // creating the image-output texture
            Run,            // the runner's dispatch
        };
        DispatchStage LastFailureStage() const { return m_lastFailure; }
        // "creating the compute shader" and so on; empty for None.
        static const wchar_t* DescribeStage(DispatchStage stage);

        // ---- IEngineComputeOutput ----
        HRESULT STDMETHODCALLTYPE GetAnalysisSrv(
            ID3D11ShaderResourceView** out) override;
        UINT64 STDMETHODCALLTYPE GetLastEvaluatedFrame() override
        { return m_lastEvaluatedFrame; }

        // ---- IEngineComputeTexture (lane 3) ----
        // Pure delegation to the runner, which owns the texture. The bridge
        // is the only thing the evaluator can QI -- D2D's outer ID2D1Effect
        // does not forward arbitrary IIDs to the impl -- so every lane has to
        // surface here even though none of them live here.
        HRESULT STDMETHODCALLTYPE RequestAnalysisTexture() override;
        HRESULT STDMETHODCALLTYPE GetAnalysisTexture(
            ID3D11Texture2D** out) override;
        HRESULT STDMETHODCALLTYPE GetAnalysisTextureSrv(
            ID3D11ShaderResourceView** out) override;

    private:
        CustomComputeBridgeEffect() = default;
        ~CustomComputeBridgeEffect() = default;

        // D2D hands back no impl, so CreateFactory leaves the new instance
        // here for Create. Thread-local: devices create effects on more
        // than one thread.
        static thread_local CustomComputeBridgeEffect* s_lastCreated;

        LONG m_refCount{ 1 };

        // D2D-side state (transform graph, draw info, input rect).
        winrt::com_ptr<ID2D1EffectContext>   m_effectContext;
        winrt::com_ptr<ID2D1TransformGraph>  m_transformGraph;
        winrt::com_ptr<ID2D1DrawInfo>        m_drawInfo;
        D2D1_RECT_L                          m_inputRect{};

        // D3D11 compute side, owned by this effect.
        Rendering::D3D11ComputeRunner m_runner;
        std::vector<BYTE>             m_pendingBytecode;
        bool                          m_bytecodeDirty{ false };
        bool                          m_asyncReadback{ false };

        // DXBC checksum identity of the pending / installed bytecode, and a
        // few previously created shaders so a baseline <-> variant switch
        // does not recreate one.
        using ShaderId = std::array<uint8_t, 16>;
        static ShaderId IdOf(const BYTE* bytes, size_t size);
        ShaderId m_pendingId{};
        ShaderId m_installedId{};
        bool     m_hasInstalled{ false };
        struct ShaderVariant
        {
            ShaderId                             id{};
            winrt::com_ptr<ID3D11ComputeShader>  shader;
        };
        std::vector<ShaderVariant> m_shaderVariants;

        // Image-output side. Populated by `Dispatch` when
        // `imageOutputW > 0 && imageOutputH > 0`. The bitmap wraps a
        // dedicated D3D11 texture the compute writes to via
        // `RWTexture2D<float4> ImageOutput : register(u1)`.
        winrt::com_ptr<ID3D11Texture2D> m_imageOutputTex;
        winrt::com_ptr<ID2D1Bitmap1>    m_imageOutput;
        UINT32                          m_imageOutputW{ 0 };
        UINT32                          m_imageOutputH{ 0 };

        // FP32 copies of the inputs, one per slot (t0..tN-1), reused while
        // an input's size holds (a 4K FP32 bitmap is 133 MB).
        struct CachedInputBitmap {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            UINT32 width{ 0 };
            UINT32 height{ 0 };
        };
        std::vector<CachedInputBitmap> m_inputBitmaps;

        UINT64 m_lastEvaluatedFrame{ 0 };

        // GPU-binding SRVs for the next Dispatch, cleared at its end.
        std::vector<ID3D11ShaderResourceView*> m_gpuBindingSrvs;
        std::vector<UINT32>                    m_gpuBindingSlots;

        // Dispatch dimensions for the next call. Default (1,1,1).
        UINT32 m_dispatchX{ 1 };
        UINT32 m_dispatchY{ 1 };
        UINT32 m_dispatchZ{ 1 };

        // Bytecode CreateComputeShader refused, so a node stuck on it fails
        // fast every frame instead of retrying (and copying its inputs).
        ShaderId m_failedShaderId{};
        HRESULT  m_failedShaderHr{ S_OK };
        bool     m_hasFailedShader{ false };
        DispatchStage m_lastFailure{ DispatchStage::None };

        // Dispatch steps.
        HRESULT Fail(DispatchStage stage, HRESULT hr);
        static HRESULT D3D11DeviceOf(ID2D1DeviceContext5* dc, winrt::com_ptr<ID3D11Device>& device);
        // Creates (or reuses) the pending shader and hands it to the runner.
        HRESULT InstallPendingShader(ID3D11Device* device);
        // The input in `slot` as an FP32 texture: its own surface when it is
        // already one, else a copy drawn into the slot's cached bitmap.
        // Needs the context at 96 DPI.
        HRESULT PrepareInputTexture(ID2D1DeviceContext5* dc, ID2D1Image* image, UINT32 slot,
                                    winrt::com_ptr<ID3D11Texture2D>& texture);
        Rendering::D3D11ComputeRunner::Readback ChooseReadback(bool wantCpu) const;

        HRESULT EnsurePassThroughShader();
        HRESULT EnsureImageOutputTexture(
            ID3D11Device* device,
            ID2D1DeviceContext5* dc,
            UINT32 w, UINT32 h);
    };
}
