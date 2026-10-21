#include "pch_engine.h"
#include "D3D11ComputeRunner.h"
#include "../Effects/ShaderLabParamsHlsl.h"
#include "../Effects/ShaderCompiler.h"

namespace ShaderLab::Rendering
{
    HRESULT __stdcall D3D11ComputeRunner::QueryInterface(REFIID iid, void** out) noexcept
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) ||
            iid == Effects::IID_IEngineComputeOutput)
        {
            // Disambiguate explicitly: the runner now inherits two IUnknown
            // paths, so a plain `this` would be ambiguous and, worse, an
            // implicit pick could hand back the wrong vtable.
            *out = static_cast<Effects::IEngineComputeOutput*>(this);
            // No-op AddRef: the owning bridge effect keeps the runner alive.
            return S_OK;
        }
        if (iid == Effects::IID_IEngineComputeTexture)
        {
            *out = static_cast<Effects::IEngineComputeTexture*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisSrv(ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        if (!m_resultSRV) return E_NOT_VALID_STATE;
        *out = m_resultSRV.get();
        (*out)->AddRef();
        return S_OK;
    }

    void D3D11ComputeRunner::Initialize(ID3D11Device* device)
    {
        if (!device) return;
        m_device.copy_from(device);
        m_device->GetImmediateContext(m_immediate.put());

        // Record on a PRIVATE deferred context, submit as one command list.
        //
        // The immediate context is shared with Direct2D -- the render
        // thread's and the UI thread's D2D contexts both draw through it --
        // and its shader / SRV / UAV bindings are context state, not
        // per-caller state. SetMultithreadProtected serializes individual
        // calls, not sequences, so a dispatch issued as the usual five calls
        // (clear, set shader, set SRVs, set UAVs, Dispatch) could have another
        // thread's work land in the middle and run with its bindings gone:
        // ~8% of re-dispatches wrote nothing while returning S_OK. Recording
        // here and submitting with a single ExecuteCommandList makes the
        // whole dispatch one call, which multithread protection DOES make
        // atomic -- with no lock, and no knowledge that D2D exists at all.
        if (FAILED(m_device->CreateDeferredContext(0, m_deferredCtx.put())))
            m_deferredCtx = nullptr;
        m_context = m_deferredCtx ? m_deferredCtx : m_immediate;
        m_deferred = (m_context == m_deferredCtx);
    }

    void D3D11ComputeRunner::SubmitRecorded()
    {
        if (!m_deferred || !m_context || !m_immediate) return;
        winrt::com_ptr<ID3D11CommandList> list;
        // FALSE: the deferred context starts the next recording from default
        // state, which is what we want -- every dispatch binds what it uses.
        if (FAILED(m_context->FinishCommandList(FALSE, list.put())) || !list)
            return;
        // TRUE is NOT optional. With FALSE the runtime resets the immediate
        // context to defaults afterwards, and if this submit lands in the
        // middle of D2D building its own pipeline state, that reset wipes
        // D2D's half-set bindings -- the same race, pointed the other way.
        // TRUE leaves the immediate context exactly as this call found it.
        m_immediate->ExecuteCommandList(list.get(), TRUE);
    }

    bool D3D11ComputeRunner::CompileShader(const std::string& hlslSource)
    {
        m_shader = nullptr;
        m_bytecode.clear();
        m_compileError.clear();

        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(
            hlslSource.c_str(), hlslSource.size(),
            "D3D11ComputeEffect", nullptr, Effects::ShaderLabIncludeHandler(),
            "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());

        if (FAILED(hr))
        {
            if (errors)
            {
                std::string msg(static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
                m_compileError = std::wstring(msg.begin(), msg.end());
            }
            else
            {
                m_compileError = L"D3DCompile failed with unknown error.";
            }
            return false;
        }

        hr = m_device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, m_shader.put());
        if (FAILED(hr))
        {
            m_compileError = L"CreateComputeShader failed.";
            return false;
        }

        // Stash bytecode so callers can run D3DReflect for cbuffer layout.
        const auto* src = static_cast<const uint8_t*>(blob->GetBufferPointer());
        m_bytecode.assign(src, src + blob->GetBufferSize());
        ReflectContracts();

        return true;
    }


    // -----------------------------------------------------------------------
    // Lane 3: analysis values as a 1 x N RGBA32F texture
    // -----------------------------------------------------------------------
    //
    // Exists because Direct2D will not let a custom PIXEL shader bind an
    // arbitrary SRV: it maps a transform's INPUTS to Texture2D at t0, t1, ...
    // and nothing else. ID2D1EffectContext::CreateResourceTexture looks like
    // the way round and is not -- it takes `const BYTE*` CPU memory, so it
    // would reintroduce the GPU->CPU readback this lane exists to remove.
    //
    // A buffer cannot be CopyResource'd into a texture (the API will not
    // convert dimensions), so the copy is a one-line compute pass. It is N
    // threads over a handful of texels; the cost is the dispatch, not the work.

    namespace
    {
        // Result[] -> row 0 of a 1 x N RGBA32F texture, texel i = Result[i].
        constexpr const char* kAnalysisCopyHLSL = R"HLSL(
StructuredBuffer<float4>   Src : register(t0);
RWTexture2D<float4>        Dst : register(u0);
cbuffer C : register(b0) { uint Count; uint3 _pad; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Count) return;
    Dst[int2((int)tid.x, 0)] = Src[tid.x];
}
)HLSL";
    }

    HRESULT __stdcall D3D11ComputeRunner::RequestAnalysisTexture()
    {
        // Recorded even before Initialize: the bridge initializes the runner
        // inside the dispatch that fills the texture, after the request.
        m_analysisTexWanted = true;
        return S_OK;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisTexture(ID3D11Texture2D** out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!m_analysisTex || !m_analysisTexFilled) return E_NOT_VALID_STATE;
        *out = m_analysisTex.get();
        (*out)->AddRef();
        return S_OK;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisTextureSrv(
        ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!m_analysisTexSRV || !m_analysisTexFilled) return E_NOT_VALID_STATE;
        *out = m_analysisTexSRV.get();
        (*out)->AddRef();
        return S_OK;
    }

    bool D3D11ComputeRunner::EnsureAnalysisTexture()
    {
        if (!m_analysisTexWanted || !m_device || m_resultCount == 0) return false;
        if (m_analysisTex && m_analysisCopyShader && m_copyConstants) return true;

        m_analysisTex = nullptr;
        m_analysisTexUAV = nullptr;
        m_analysisTexSRV = nullptr;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_resultCount;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE so a consumer can read it; UNORDERED_ACCESS so the
        // copy pass can write it. No CPU access on purpose -- the entire point
        // is that these values never touch the CPU.
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_analysisTex.put())))
        {
            m_analysisTexWanted = false;   // do not retry every frame
            return false;
        }
        if (FAILED(m_device->CreateUnorderedAccessView(
                m_analysisTex.get(), nullptr, m_analysisTexUAV.put())) ||
            FAILED(m_device->CreateShaderResourceView(
                m_analysisTex.get(), nullptr, m_analysisTexSRV.put())))
        {
            m_analysisTex = nullptr;
            m_analysisTexWanted = false;
            return false;
        }

        if (!m_analysisCopyShader)
        {
            winrt::com_ptr<ID3DBlob> blob, errors;
            HRESULT hr = D3DCompile(
                kAnalysisCopyHLSL, std::strlen(kAnalysisCopyHLSL),
                "AnalysisCopy", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, blob.put(), errors.put());
            if (FAILED(hr) || !blob)
            {
                m_analysisTexWanted = false;
                return false;
            }
            if (FAILED(m_device->CreateComputeShader(
                    blob->GetBufferPointer(), blob->GetBufferSize(),
                    nullptr, m_analysisCopyShader.put())))
            {
                m_analysisTexWanted = false;
                return false;
            }
        }

        // The copy pass's count, fixed for this texture.
        const UINT copyConstants[4] = { m_resultCount, 0, 0, 0 };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(copyConstants);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{ copyConstants, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&bd, &init, m_copyConstants.put())))
        {
            m_analysisTex = nullptr;
            m_analysisTexWanted = false;
            return false;
        }
        return true;
    }

    void D3D11ComputeRunner::PublishAnalysisTexture()
    {
        if (!EnsureAnalysisTexture()) return;
        if (!m_resultSRV || !m_analysisTexUAV || !m_copyConstants || !m_context) return;

        ID3D11ShaderResourceView* srvs[] = { m_resultSRV.get() };
        ID3D11UnorderedAccessView* uavs[] = { m_analysisTexUAV.get() };
        // Its own cbuffer: the main shader's constants have a different
        // layout, and rebinding theirs here would corrupt the next dispatch.
        ID3D11Buffer* cbs[] = { m_copyConstants.get() };
        UINT counts[] = { 0 };

        m_context->CSSetShader(m_analysisCopyShader.get(), nullptr, 0);
        m_context->CSSetShaderResources(0, 1, srvs);
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, counts);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch((m_resultCount + 63) / 64, 1, 1);

        // Unbind: leaving the result SRV bound to t0 would conflict with the
        // next dispatch binding the same resource as a UAV, and D3D silently
        // drops one of the two.
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSrv);
        m_context->CSSetUnorderedAccessViews(0, 1, nullUav, counts);
        m_context->CSSetShader(nullptr, nullptr, 0);

        m_analysisTexFilled = true;
    }

    HRESULT D3D11ComputeRunner::WriteResults(uint32_t firstElement, const float* float4s, uint32_t elementCount)
    {
        if (!m_resultBuffer || !m_immediate || !float4s || elementCount == 0 ||
            uint64_t{ firstElement } + elementCount > m_resultCount)
            return E_INVALIDARG;
        // On the immediate context: one call, which multithread protection
        // keeps whole; the copy pass that follows is recorded and submitted
        // after it.
        const D3D11_BOX box{ firstElement * 16u, 0, 0, (firstElement + elementCount) * 16u, 1, 1 };
        m_immediate->UpdateSubresource(m_resultBuffer.get(), 0, &box, float4s, 0, 0);
        if (m_analysisTexWanted && m_analysisTexFilled)
        {
            PublishAnalysisTexture();
            SubmitRecorded();
        }
        return S_OK;
    }

    HRESULT D3D11ComputeRunner::ReadResults(std::vector<float>& out)
    {
        if (!m_resultBuffer || !m_immediate || m_resultCount == 0)
            return E_NOT_VALID_STATE;
        // The dispatch has been submitted, so the immediate context orders
        // this copy after it.
        StagingSlot* slot = ScheduleReadback(m_immediate.get());
        if (!slot) return E_NOT_VALID_STATE;
        return CollectBlocking(*slot, m_resultCount, out);
    }

    HRESULT D3D11ComputeRunner::EnsureBuffers(uint32_t resultCount)
    {
        if (m_resultCount == resultCount && m_resultBuffer) return S_OK;
        m_resultCount = resultCount;
        // Lane 3 is sized off resultCount, so it has to go with the rest.
        // Keep m_analysisTexWanted: the consumer's request outlives a resize.
        m_analysisTex = nullptr;
        m_analysisTexUAV = nullptr;
        m_analysisTexSRV = nullptr;
        m_copyConstants = nullptr;
        m_analysisTexFilled = false;
        m_resultBuffer = nullptr;
        for (auto& slot : m_staging) { slot.buffer = nullptr; slot.pending = false; }
        m_resultUAV = nullptr;
        m_resultSRV = nullptr;

        // Any failure leaves m_resultBuffer null, so the next call retries.
        auto fail = [this](HRESULT hr)
        {
            m_resultBuffer = nullptr;
            m_resultUAV = nullptr;
            m_resultSRV = nullptr;
            for (auto& slot : m_staging) slot.buffer = nullptr;
            return hr;
        };

        const uint32_t byteSize = (std::max)(resultCount * 16u, 16u); // 16 bytes per float4

        // The result buffer is written through a UAV and read by downstream
        // GPU consumers through an SRV (IEngineComputeOutput), without Map().
        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = byteSize;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = 16; // sizeof(float4)
        if (const HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, m_resultBuffer.put()); FAILED(hr))
            return fail(hr);

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = resultCount;
        if (const HRESULT hr = m_device->CreateUnorderedAccessView(m_resultBuffer.get(), &uavDesc, m_resultUAV.put()); FAILED(hr))
            return fail(hr);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = resultCount;
        if (const HRESULT hr = m_device->CreateShaderResourceView(m_resultBuffer.get(), &srvDesc, m_resultSRV.put()); FAILED(hr))
            return fail(hr);

        // Staging ring for readback.
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.BindFlags = 0;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        for (auto& slot : m_staging)
            if (const HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, slot.buffer.put()); FAILED(hr))
                return fail(hr);
        return S_OK;
    }

    void D3D11ComputeRunner::InstallPrecompiledShader(
        const std::vector<uint8_t>& bytecode,
        winrt::com_ptr<ID3D11ComputeShader> shader)
    {
        // Install pre-compiled bytecode + the live shader handle. Used
        // by CustomComputeBridgeEffect to bypass the runtime D3DCompile
        // path when the bytecode is already produced by the host (e.g.
        // an MCP /effect/compile call). Subsequent Dispatch calls use
        // the installed shader directly.
        m_bytecode = bytecode;
        m_shader = std::move(shader);
        m_compileError.clear();
        ReflectContracts();
    }

    void D3D11ComputeRunner::ReflectContracts()
    {
        m_usesScratch = false;
        m_usesImagePass = false;
        m_accumulatorStride = 0;
        m_accumulatorNeedsClear = true;
        m_groupSize[0] = m_groupSize[1] = m_groupSize[2] = 1;
        winrt::com_ptr<ID3D11ShaderReflection> reflect;
        if (m_bytecode.empty() ||
            FAILED(D3DReflect(m_bytecode.data(), m_bytecode.size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()))) || !reflect)
            return;

        reflect->GetThreadGroupSize(&m_groupSize[0], &m_groupSize[1], &m_groupSize[2]);

        // Multi-group reduction contract.
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (SUCCEEDED(reflect->GetResourceBindingDescByName("_SLScratch", &bd)))
            m_usesScratch = (bd.BindPoint == 2);

        // Two-pass image contract. For a structured buffer, NumSamples is the stride.
        D3D11_SHADER_INPUT_BIND_DESC passBind{};
        D3D11_SHADER_INPUT_BIND_DESC accumulatorBind{};
        if (SUCCEEDED(reflect->GetResourceBindingDescByName("_SLPassConstants", &passBind)) &&
            passBind.Type == D3D_SIT_CBUFFER && passBind.BindPoint == Effects::cImagePassConstantsSlot &&
            SUCCEEDED(reflect->GetResourceBindingDescByName("_SLPixelAccum", &accumulatorBind)) &&
            accumulatorBind.Type == D3D_SIT_UAV_RWSTRUCTURED &&
            accumulatorBind.BindPoint == Effects::cImagePassAccumulatorSlot &&
            accumulatorBind.NumSamples > 0)
        {
            m_usesImagePass = true;
            m_accumulatorStride = accumulatorBind.NumSamples;
        }
    }

    HRESULT D3D11ComputeRunner::EnsureImagePassResources(uint64_t pixelCount)
    {
        if (!m_device) return E_NOT_VALID_STATE;
        if (!m_passConstants[0])
        {
            for (UINT pass = 0; pass < 2; ++pass)
            {
                const UINT data[4] = { pass, 0, 0, 0 };
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = sizeof(data);
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
                const HRESULT hr = m_device->CreateBuffer(&desc, &init, m_passConstants[pass].put());
                if (FAILED(hr))
                {
                    m_passConstants[0] = nullptr;
                    m_passConstants[1] = nullptr;
                    return hr;
                }
            }
        }

        const uint64_t bytes = pixelCount * m_accumulatorStride;
        if (m_accumulatorUAV && m_accumulatorBytes == bytes) return S_OK;
        m_accumulator = nullptr;
        m_accumulatorUAV = nullptr;
        m_accumulatorBytes = 0;
        if (pixelCount == 0 || bytes > UINT32_MAX) return E_OUTOFMEMORY;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(bytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = m_accumulatorStride;
        HRESULT hr = m_device->CreateBuffer(&desc, nullptr, m_accumulator.put());
        if (FAILED(hr)) return hr;

        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = static_cast<UINT>(pixelCount);
        hr = m_device->CreateUnorderedAccessView(m_accumulator.get(), &view, m_accumulatorUAV.put());
        if (FAILED(hr))
        {
            m_accumulator = nullptr;
            return hr;
        }
        m_accumulatorBytes = bytes;
        m_accumulatorNeedsClear = true;
        return S_OK;
    }

    bool D3D11ComputeRunner::EnsureScratch()
    {
        if (m_scratchUAV) return true;
        if (!m_device) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = Effects::kReduceScratchUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, m_scratch.put()))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = Effects::kReduceScratchUints;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(m_device->CreateUnorderedAccessView(m_scratch.get(), &ud, m_scratchUAV.put())))
            return false;
        ud.Buffer.NumElements = Effects::kReduceScratchClearedUints;
        if (FAILED(m_device->CreateUnorderedAccessView(m_scratch.get(), &ud, m_scratchHeadUAV.put())))
        {
            m_scratchUAV = nullptr;
            return false;
        }
        return true;
    }

    bool D3D11ComputeRunner::PollReadback(std::vector<float>& out)
    {
        if (!m_immediate || m_resultCount == 0) return false;
        // Newest completed copy wins; walk newest-first so the first success
        // ends the search, then retire everything older.
        std::array<StagingSlot*, kStagingSlots> order{};
        size_t n = 0;
        for (auto& s : m_staging) if (s.pending) order[n++] = &s;
        std::sort(order.begin(), order.begin() + n,
            [](const StagingSlot* a, const StagingSlot* b) { return a->seq > b->seq; });
        for (size_t i = 0; i < n; ++i)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_immediate->Map(order[i]->buffer.get(), 0, D3D11_MAP_READ,
                                          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr))
            {
                const float* data = static_cast<const float*>(mapped.pData);
                out.assign(data, data + m_resultCount * 4);
                m_immediate->Unmap(order[i]->buffer.get(), 0);
            }
            // Completed (or failed for good): this slot and every older one
            // are done -- older values are superseded by this one.
            const uint64_t seq = order[i]->seq;
            for (auto& s : m_staging)
                if (s.pending && s.seq <= seq) s.pending = false;
            return SUCCEEDED(hr);
        }
        return false;
    }

    std::vector<float> D3D11ComputeRunner::Dispatch(
        ID3D11Texture2D* inputTexture,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount)
    {
        std::vector<ID3D11Texture2D*> inputs{ inputTexture };
        return DispatchWithImageOutput(inputs, cbufferData, resultCount, nullptr);
    }

    HRESULT D3D11ComputeRunner::CreateInputViews(const std::vector<ID3D11Texture2D*>& inputTextures,
                                                 std::vector<winrt::com_ptr<ID3D11ShaderResourceView>>& views)
    {
        views.clear();
        views.reserve(inputTextures.size());
        for (auto* tex : inputTextures)
        {
            if (!tex) return E_INVALIDARG;
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = d.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            if (const HRESULT hr = m_device->CreateShaderResourceView(tex, &srvDesc, srv.put()); FAILED(hr))
                return hr;
            views.push_back(std::move(srv));
        }
        return S_OK;
    }

    HRESULT D3D11ComputeRunner::CreateImageOutputView(ID3D11Texture2D* imageOutputTexture,
                                                      winrt::com_ptr<ID3D11UnorderedAccessView>& view,
                                                      D3D11_TEXTURE2D_DESC& desc)
    {
        if (!imageOutputTexture) return S_OK;
        imageOutputTexture->GetDesc(&desc);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (const HRESULT hr = m_device->CreateUnorderedAccessView(imageOutputTexture, &uavDesc, view.put()); FAILED(hr))
            return hr;
        // Cleared, so a shader that writes selectively leaks no earlier frame.
        const float clear[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewFloat(view.get(), clear);
        return S_OK;
    }

    HRESULT D3D11ComputeRunner::EnsureConstantBuffer(UINT userBytes)
    {
        // At least 256 bytes; more when the user constants need it.
        const UINT needed = (std::max)(cMinConstantBufferBytes, (cRunnerConstantBytes + userBytes + 15u) & ~15u);
        if (m_cbuffer && m_cbufferBytes >= needed) return S_OK;
        if (needed > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u) return E_INVALIDARG;
        m_cbuffer = nullptr;
        m_cbufferBytes = 0;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = needed;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (const HRESULT hr = m_device->CreateBuffer(&desc, nullptr, m_cbuffer.put()); FAILED(hr))
            return hr;
        m_cbufferBytes = needed;
        return S_OK;
    }

    HRESULT D3D11ComputeRunner::PackConstants(const D3D11_TEXTURE2D_DESC& input0, const std::vector<BYTE>& cbufferData)
    {
        if (const HRESULT hr = EnsureConstantBuffer(static_cast<UINT>(cbufferData.size())); FAILED(hr))
            return hr;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (const HRESULT hr = m_context->Map(m_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped); FAILED(hr))
            return hr;
        auto* cb = static_cast<BYTE*>(mapped.pData);
        std::memset(cb, 0, m_cbufferBytes);
        // Width and Height of input 0, then the user constants.
        const uint32_t dims[2] = { input0.Width, input0.Height };
        std::memcpy(cb, dims, cRunnerConstantBytes);
        if (!cbufferData.empty())
            std::memcpy(cb + cRunnerConstantBytes, cbufferData.data(), cbufferData.size());
        m_context->Unmap(m_cbuffer.get(), 0);
        return S_OK;
    }

    void D3D11ComputeRunner::RunPasses(const std::vector<ID3D11ShaderResourceView*>& inputViews,
                                       ID3D11UnorderedAccessView* imageView,
                                       const D3D11_TEXTURE2D_DESC& input0, const D3D11_TEXTURE2D_DESC& output,
                                       uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ,
                                       const std::vector<ID3D11ShaderResourceView*>& extraSrvs,
                                       const std::vector<uint32_t>& extraSrvSlots)
    {
        const uint32_t clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_resultUAV.get(), clearValues);

        m_context->CSSetShader(m_shader.get(), nullptr, 0);
        m_context->CSSetShaderResources(0, static_cast<UINT>(inputViews.size()), inputViews.data());

        // GPU-bound parameters: each upstream analysis SRV at its own t-slot,
        // bound one by one so the slots need not be contiguous.
        const size_t extraCount = (std::min)(extraSrvs.size(), extraSrvSlots.size());
        for (size_t i = 0; i < extraCount; ++i)
        {
            ID3D11ShaderResourceView* one[1] = { extraSrvs[i] };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, one);
        }

        // Multi-group reduction: scratch at u2, head zeroed, and a
        // kReduceGroups-wide dispatch unless the caller sized it.
        const bool scratch = m_usesScratch && EnsureScratch();
        if (scratch)
        {
            const UINT zero[4] = { 0, 0, 0, 0 };
            m_context->ClearUnorderedAccessViewUint(m_scratchHeadUAV.get(), zero);
            if (!m_usesImagePass && dispatchX <= 1 && dispatchY <= 1 && dispatchZ <= 1)
                dispatchX = Effects::kReduceGroups;
        }

        ID3D11UnorderedAccessView* uavs[4] = { m_resultUAV.get(), imageView,
                                               scratch ? m_scratchUAV.get() : nullptr,
                                               m_usesImagePass ? m_accumulatorUAV.get() : nullptr };
        const UINT uavCount = m_usesImagePass ? 4u : (scratch ? 3u : (imageView ? 2u : 1u));
        m_context->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);
        ID3D11Buffer* cbs[] = { m_cbuffer.get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        if (m_usesImagePass)
        {
            // Pass 0 over input 0, pass 1 over the output. D3D11 orders the
            // two dispatches, so pass 1 sees every pass-0 atomic. Pass 1
            // leaves the accumulator zeroed, so it is cleared here only when
            // nothing has run on it yet.
            if (m_accumulatorNeedsClear)
            {
                const UINT zero[4] = { 0, 0, 0, 0 };
                m_context->ClearUnorderedAccessViewUint(m_accumulatorUAV.get(), zero);
                m_accumulatorNeedsClear = false;
            }
            ID3D11Buffer* passZero[] = { m_passConstants[0].get() };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, passZero);
            m_context->Dispatch(
                (input0.Width + m_groupSize[0] - 1) / m_groupSize[0],
                (input0.Height + m_groupSize[1] - 1) / m_groupSize[1], 1);
            ID3D11Buffer* passOne[] = { m_passConstants[1].get() };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, passOne);
            m_context->Dispatch(
                (output.Width + m_groupSize[0] - 1) / m_groupSize[0],
                (output.Height + m_groupSize[1] - 1) / m_groupSize[1], 1);
            ID3D11Buffer* noPass[] = { nullptr };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, noPass);
        }
        else
        {
            m_context->Dispatch((std::max)(dispatchX, 1u), (std::max)(dispatchY, 1u), (std::max)(dispatchZ, 1u));
        }

        // Unbind everything this dispatch bound.
        std::vector<ID3D11ShaderResourceView*> nullSrvs(inputViews.size(), nullptr);
        m_context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
        for (size_t i = 0; i < extraCount; ++i)
        {
            ID3D11ShaderResourceView* none[1] = { nullptr };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, none);
        }
        ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, uavCount, nullUAVs, nullptr);
        m_context->CSSetShader(nullptr, nullptr, 0);
    }

    D3D11ComputeRunner::StagingSlot* D3D11ComputeRunner::ScheduleReadback(ID3D11DeviceContext* context)
    {
        // A free slot; with all of them in flight, the oldest (its values
        // are superseded by this copy anyway).
        StagingSlot* slot = nullptr;
        for (auto& s : m_staging)
            if (s.buffer && !s.pending) { slot = &s; break; }
        if (!slot)
        {
            for (auto& s : m_staging)
                if (s.buffer && (!slot || s.seq < slot->seq)) slot = &s;
        }
        if (slot)
            context->CopyResource(slot->buffer.get(), m_resultBuffer.get());
        return slot;
    }

    HRESULT D3D11ComputeRunner::CollectBlocking(StagingSlot& slot, uint32_t resultCount, std::vector<float>& out)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = m_immediate->Map(slot.buffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            const float* data = static_cast<const float*>(mapped.pData);
            out.assign(data, data + resultCount * 4);
            m_immediate->Unmap(slot.buffer.get(), 0);
        }
        // Anything still in flight is older than this read, which supersedes it.
        for (auto& s : m_staging) s.pending = false;
        return hr;
    }

    std::vector<float> D3D11ComputeRunner::DispatchWithImageOutput(
        const std::vector<ID3D11Texture2D*>& inputTextures,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount,
        ID3D11Texture2D* imageOutputTexture,
        uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ,
        const std::vector<ID3D11ShaderResourceView*>& extraSrvs,
        const std::vector<uint32_t>& extraSrvSlots,
        Readback readbackMode)
    {
        std::vector<float> result;
        m_lastDispatchResult = E_FAIL;
        m_lastReadbackResult = S_OK;
        auto fail = [&](HRESULT hr) { m_lastDispatchResult = hr; return std::move(result); };

        // Record on the private deferred context unless the benchmark switch
        // says otherwise (UseImmediateContextForBenchmark).
        m_context = (m_deferredCtx && !m_immediateForBenchmark) ? m_deferredCtx : m_immediate;
        m_deferred = (m_context == m_deferredCtx) && m_deferredCtx;
        if (!m_shader || !m_context || inputTextures.empty() || !inputTextures[0])
            return fail(E_NOT_VALID_STATE);

        // Width / Height come from input 0: multi-input shaders work on
        // inputs of one size, and upstream matches them before binding.
        D3D11_TEXTURE2D_DESC input0{};
        inputTextures[0]->GetDesc(&input0);

        // At least one result slot, so a shader with no analysis output still
        // has a valid buffer bound.
        if (const HRESULT hr = EnsureBuffers((std::max)(resultCount, 1u)); FAILED(hr))
            return fail(hr);

        std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> inputViews;
        if (const HRESULT hr = CreateInputViews(inputTextures, inputViews); FAILED(hr))
            return fail(hr);
        std::vector<ID3D11ShaderResourceView*> inputViewPtrs;
        inputViewPtrs.reserve(inputViews.size());
        for (const auto& view : inputViews) inputViewPtrs.push_back(view.get());

        winrt::com_ptr<ID3D11UnorderedAccessView> imageView;
        D3D11_TEXTURE2D_DESC output{};
        if (const HRESULT hr = CreateImageOutputView(imageOutputTexture, imageView, output); FAILED(hr))
            return fail(hr);

        // The two-pass image contract needs an image to write and its accumulator.
        if (m_usesImagePass)
        {
            if (!imageView) return fail(E_INVALIDARG);
            const HRESULT hr = EnsureImagePassResources(uint64_t{ output.Width } * output.Height);
            if (FAILED(hr)) return fail(hr);
            if (!EnsureScratch()) return fail(E_OUTOFMEMORY);
        }

        if (const HRESULT hr = PackConstants(input0, cbufferData); FAILED(hr))
            return fail(hr);

        RunPasses(inputViewPtrs, imageView.get(), input0, output, dispatchX, dispatchY, dispatchZ,
                  extraSrvs, extraSrvSlots);

        // Lane 3: publish Result[] into the 1 x N texture only when a consumer
        // asked for it -- a graph with no pixel-shader consumer never creates
        // the texture or runs the copy. After the unbind above: the copy reads
        // the result buffer as an SRV, and D3D silently drops one of two
        // views of a resource bound at once.
        if (m_analysisTexWanted && resultCount > 0)
            PublishAnalysisTexture();

        // Readback only when the caller wants the values on the CPU; the SRV
        // sees the fresh buffer either way.
        StagingSlot* slot = (resultCount > 0 && readbackMode != Readback::None) ? ScheduleReadback(m_context.get()) : nullptr;

        // Everything above goes to the GPU as ONE call.
        SubmitRecorded();

        // A deferred context cannot Map for READ, so the read happens on the
        // immediate context, after the list that fills the staging buffer.
        if (slot && readbackMode == Readback::Blocking)
        {
            m_lastReadbackResult = CollectBlocking(*slot, resultCount, result);
        }
        else if (slot)
        {
            slot->pending = true;
            slot->seq = ++m_stagingSeq;
        }

        // Consumers detect a fresh dispatch by the counter.
        ++m_lastEvaluatedFrame;
        m_lastDispatchResult = S_OK;
        return result;
    }
}
