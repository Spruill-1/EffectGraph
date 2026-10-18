#pragma once

#include "pch_engine.h"
#include "../Effects/IEngineComputeOutput.h"
#include "../Effects/IEngineComputeTexture.h"

namespace ShaderLab::Rendering
{
    // Generic D3D11 compute shader dispatch runner for user-authored shaders.
    // Handles: shader compilation, SRV/UAV/CB creation, dispatch, and readback.
    //
    // Output uses RWStructuredBuffer<float4> matching the analysisFields ABI:
    //   - One float4 per analysis pixel (field.pixelCount())
    //   - Float → .x, Float2 → .xy, Float3 → .xyz, Float4 → all
    //   - Compatible with existing ReadCustomAnalysisOutput readback
    //
    // cbuffer contract: first 8 bytes = uint Width, uint Height (auto-injected).
    // User parameters start at offset 8.
    //
    // Implements IEngineComputeOutput (lane 1) and IEngineComputeTexture
    // (lane 3) so downstream effects read the results on the GPU without a
    // Map(); the lanes are defined in docs/architecture/d2d-d3d11-hybrid-compute.md.
    // The COM implementation is no-op-refcounted: the runner is an interior
    // object of the bridge effect that holds it by value, valid while that
    // effect lives.
    class D3D11ComputeRunner
        : public Effects::IEngineComputeOutput
        , public Effects::IEngineComputeTexture
    {
    public:
        D3D11ComputeRunner() = default;
        virtual ~D3D11ComputeRunner() = default;

        // Initialize with a D3D11 device (caches device + immediate context).
        void Initialize(ID3D11Device* device);

        // Compile a compute shader from HLSL source.
        // Returns true on success. Call GetCompileError() on failure.
        // On success, GetCompiledBytecode() returns the bytecode blob —
        // the caller should store it on the CustomEffectDefinition so
        // cbuffer reflection can pack user properties at the right offsets.
        bool CompileShader(const std::string& hlslSource);

        // Install bytecode compiled elsewhere (the bridge effect's path:
        // the evaluator compiles through the BytecodeCache). The runner
        // keeps the blob and the live ID3D11ComputeShader; later Dispatch
        // calls use them as they would CompileShader's output.
        void InstallPrecompiledShader(
            const std::vector<uint8_t>& bytecode,
            winrt::com_ptr<ID3D11ComputeShader> shader);

        // Get the last compile error message.
        const std::wstring& GetCompileError() const { return m_compileError; }

        // Get the compiled bytecode (empty until CompileShader succeeds).
        const std::vector<uint8_t>& GetCompiledBytecode() const { return m_bytecode; }

        // Dispatch the compiled shader on an input texture.
        // cbufferData: user constant buffer bytes (Width/Height prepended automatically).
        // resultCount: number of float4 elements in the result buffer.
        // Returns the readback data (resultCount * 4 floats).
        std::vector<float> Dispatch(
            ID3D11Texture2D* inputTexture,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount);

        // Dispatch with an optional secondary image-output texture
        // bound at u1 as `RWTexture2D<float4>`. Used by
        // CustomComputeBridgeEffect: image-producing D3D11 compute
        // effects (CIE Histogram et al.) write their image output
        // here while still publishing analysis values to the
        // structured buffer at u0. Pass nullptr for analysis-only.
        // Same readback contract as the regular Dispatch.
        //
        // dispatchX/Y/Z: number of thread groups. Defaults to (1,1,1)
        // for analysis-only effects whose [numthreads] is sized to
        // cover the analysis output internally (e.g. numthreads(64,1,1)
        // running a per-thread loop over Width*Height pixels). Image-
        // producing per-pixel effects pass (W/tx, H/ty, 1) where tx,ty
        // are the [numthreads] dimensions.
        //
        // extraSrvs / extraSrvSlots: GPU bindings (lane 1). For each entry, the SRV is bound at the named t-slot before
        // dispatch. Used by the bridge to wire upstream
        // IEngineComputeOutput SRVs into a consumer's
        // SHADERLAB_GPU_BUFFER slots without a CPU readback round-trip.
        // Pass empty vectors for the no-binding case.
        //
        // readback: None leaves the values GPU-only (the SRV still sees them).
        // Blocking copies and Maps immediately -- the Map waits for the GPU to
        // finish EVERYTHING queued on the immediate context, including the
        // upstream D2D pre-render, so it drains the pipeline. Async queues the copy into a
        // small staging ring and returns nothing; PollReadback collects the
        // values on a later frame without waiting.
        enum class Readback { None, Blocking, Async };
        std::vector<float> DispatchWithImageOutput(
            const std::vector<ID3D11Texture2D*>& inputTextures,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount,
            ID3D11Texture2D* imageOutputTexture,
            uint32_t dispatchX = 1, uint32_t dispatchY = 1, uint32_t dispatchZ = 1,
            const std::vector<ID3D11ShaderResourceView*>& extraSrvs = {},
            const std::vector<uint32_t>& extraSrvSlots = {},
            Readback readback = Readback::Blocking);

        // Newest async readback that has completed, without waiting. Returns
        // false when nothing new has landed. Older completed copies are
        // discarded in favour of the newest.
        bool PollReadback(std::vector<float>& out);
        bool HasPendingReadback() const
        {
            for (const auto& s : m_staging) if (s.pending) return true;
            return false;
        }

        // Benchmark-only A/B mode: issue dispatches call by call on the shared
        // immediate context instead of recording them on the private deferred
        // context. Racy against Direct2D on other threads (see Initialize);
        // EvaluatorOptions::computeCommandList = false turns it on.
        void UseImmediateContextForBenchmark(bool on) { m_immediateForBenchmark = on; }

        bool IsInitialized() const { return m_device != nullptr; }
        bool HasShader() const { return m_shader != nullptr; }

        // Why the last Dispatch did nothing, or S_OK.
        HRESULT LastDispatchResult() const { return m_lastDispatchResult; }
        // The blocking readback's Map: a failure leaves the dispatch done and
        // the returned values empty. S_OK when no blocking read was asked for.
        HRESULT LastReadbackResult() const { return m_lastReadbackResult; }

        // True when the installed shader declares SHADERLAB_IMAGE_PASS.
        bool UsesImagePass() const { return m_usesImagePass; }

        // SRV onto the result structured buffer. Used by upstream
        // effects implementing IEngineComputeOutput to expose their
        // analysis output for direct GPU consumption by downstream
        // SHADERLAB_GPU_BUFFER bindings -- no Map() round-trip. Returns
        // nullptr until the first Dispatch (the SRV is created lazily
        // alongside the UAV in EnsureBuffers).
        ID3D11ShaderResourceView* GetResultSRV() const { return m_resultSRV.get(); }

        // ---- IUnknown (no-op refcount) ----------------------------------
        // The runner's lifetime is owned by the bridge effect holding it;
        // AddRef / Release are no-ops returning a constant, as for any
        // "interior" COM object with an external owner. Callers must not
        // outlive that effect.
        HRESULT __stdcall QueryInterface(REFIID iid, void** out) noexcept override;
        ULONG   __stdcall AddRef() noexcept override { return 1; }
        ULONG   __stdcall Release() noexcept override { return 1; }

        // ---- IEngineComputeOutput ---------------------------------------
        HRESULT __stdcall GetAnalysisSrv(ID3D11ShaderResourceView** out) override;
        UINT64  __stdcall GetLastEvaluatedFrame() override { return m_lastEvaluatedFrame; }

        // ---- IEngineComputeTexture (lane 3) ------------------------------
        HRESULT __stdcall RequestAnalysisTexture() override;
        HRESULT __stdcall GetAnalysisTexture(ID3D11Texture2D** out) override;
        HRESULT __stdcall GetAnalysisTextureSrv(ID3D11ShaderResourceView** out) override;
        // True once a consumer has asked for lane 3. Read by Dispatch to
        // decide whether to run the copy pass at all.
        bool AnalysisTextureRequested() const { return m_analysisTexWanted; }

    private:
        winrt::com_ptr<ID3D11Device> m_device;
        // The context a dispatch is issued on: the private deferred context,
        // so a whole dispatch is recorded, closed into a command list and
        // handed over with one ExecuteCommandList (SubmitRecorded). It is the
        // shared immediate context only when the device refused a deferred
        // one or the benchmark switch above is on.
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        // The shared immediate context. Touched for exactly two single calls:
        // ExecuteCommandList, and the CPU readback Map (which a deferred
        // context cannot do).
        winrt::com_ptr<ID3D11DeviceContext> m_immediate;
        // The runner's private deferred context (null if the device refused
        // one). m_context points at it or at m_immediate, chosen per dispatch.
        winrt::com_ptr<ID3D11DeviceContext> m_deferredCtx;
        bool m_deferred{ false };
        bool m_immediateForBenchmark{ false };

        // Close what has been recorded on m_context into a command list and
        // execute it on the immediate context. No-op in immediate mode.
        void SubmitRecorded();
        winrt::com_ptr<ID3D11ComputeShader> m_shader;
        std::vector<uint8_t> m_bytecode;

        // Result buffer (RWStructuredBuffer<float4>) -- bound as both
        // UAV (writer) and SRV (lane-1 consumer).
        winrt::com_ptr<ID3D11Buffer> m_resultBuffer;
        // Staging ring. Blocking reads use any slot and Map at once; async
        // reads rotate through the ring so a copy can be in flight while
        // earlier ones are collected. Three covers the usual 1-2 frame GPU
        // latency; a fourth outstanding copy overwrites the oldest.
        struct StagingSlot
        {
            winrt::com_ptr<ID3D11Buffer> buffer;
            bool     pending{ false };
            uint64_t seq{ 0 };
        };
        static constexpr size_t kStagingSlots = 3;
        std::array<StagingSlot, kStagingSlots> m_staging{};
        uint64_t m_stagingSeq{ 0 };
        // Parameter cbuffer: the runner's Width/Height, then the user
        // constants. 256 bytes unless the constants need more.
        static constexpr UINT cRunnerConstantBytes = 8;
        static constexpr UINT cMinConstantBufferBytes = 256;
        winrt::com_ptr<ID3D11Buffer> m_cbuffer;
        UINT m_cbufferBytes{ 0 };
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUAV;
        winrt::com_ptr<ID3D11ShaderResourceView>  m_resultSRV;
        uint32_t m_resultCount{ 0 };
        uint64_t m_lastEvaluatedFrame{ 0 };

        // Lane 3: Result[] copied into a 1 x N RGBA32F texture so a Direct2D
        // PIXEL shader can consume it as an effect input. Nothing here is
        // created until RequestAnalysisTexture() is called -- a graph with no
        // pixel-shader consumer pays nothing for the lane.
        bool m_analysisTexWanted{ false };
        winrt::com_ptr<ID3D11Texture2D>           m_analysisTex;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_analysisTexUAV;
        winrt::com_ptr<ID3D11ShaderResourceView>  m_analysisTexSRV;
        winrt::com_ptr<ID3D11ComputeShader>       m_analysisCopyShader;
        winrt::com_ptr<ID3D11Buffer>              m_copyConstants;   // the copy pass's count
        bool m_analysisTexFilled{ false };

        std::wstring m_compileError;

        // Multi-group reduction scratch (see ShaderLabParamsHlsl.h). Created
        // on first use by a shader that declares `_SLScratch`.
        bool m_usesScratch{ false };
        winrt::com_ptr<ID3D11Buffer>              m_scratch;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_scratchUAV;       // whole buffer
        winrt::com_ptr<ID3D11UnorderedAccessView> m_scratchHeadUAV;   // cleared head
        bool EnsureScratch();

        // Two-pass image contract (see ShaderLabParamsHlsl.h).
        bool m_usesImagePass{ false };
        UINT m_groupSize[3]{ 1, 1, 1 };
        uint32_t m_accumulatorStride{ 0 };
        uint64_t m_accumulatorBytes{ 0 };
        bool m_accumulatorNeedsClear{ true };
        winrt::com_ptr<ID3D11Buffer>              m_accumulator;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_accumulatorUAV;
        winrt::com_ptr<ID3D11Buffer>              m_passConstants[2];
        HRESULT EnsureImagePassResources(uint64_t pixelCount);
        // Reads the scratch / image-pass opt-ins and the group size.
        void ReflectContracts();
        HRESULT m_lastDispatchResult{ S_OK };
        HRESULT m_lastReadbackResult{ S_OK };

        // Result buffer, its views and the staging ring, for resultCount float4s.
        HRESULT EnsureBuffers(uint32_t resultCount);

        // DispatchWithImageOutput's steps, in order.
        HRESULT CreateInputViews(const std::vector<ID3D11Texture2D*>& inputTextures,
                                 std::vector<winrt::com_ptr<ID3D11ShaderResourceView>>& views);
        // Creates and clears the u1 view; does nothing without an output texture.
        HRESULT CreateImageOutputView(ID3D11Texture2D* imageOutputTexture,
                                      winrt::com_ptr<ID3D11UnorderedAccessView>& view,
                                      D3D11_TEXTURE2D_DESC& desc);
        HRESULT EnsureConstantBuffer(UINT userBytes);
        HRESULT PackConstants(const D3D11_TEXTURE2D_DESC& input0, const std::vector<BYTE>& cbufferData);
        // Binds, dispatches (one pass, or the image contract's two) and unbinds.
        void RunPasses(const std::vector<ID3D11ShaderResourceView*>& inputViews,
                       ID3D11UnorderedAccessView* imageView,
                       const D3D11_TEXTURE2D_DESC& input0, const D3D11_TEXTURE2D_DESC& output,
                       uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ,
                       const std::vector<ID3D11ShaderResourceView*>& extraSrvs,
                       const std::vector<uint32_t>& extraSrvSlots);
        // Copies the result buffer to a staging slot; nullptr if none exists.
        StagingSlot* ScheduleReadback();
        HRESULT CollectBlocking(StagingSlot& slot, uint32_t resultCount, std::vector<float>& out);
        // Creates the lane-3 texture + copy shader on demand. Safe to call
        // every dispatch; returns false if the lane is unavailable.
        bool EnsureAnalysisTexture();
        // Buffer -> texture copy dispatch. Runs only when the lane is
        // wanted AND available.
        void PublishAnalysisTexture();

    public:
        // Writes float4 elements into the results between dispatches, and
        // refreshes the pixel-shader copy when one is published.
        HRESULT WriteResults(uint32_t firstElement, const float* float4s, uint32_t elementCount);

    private:
    };
}
