#include "pch_engine.h"
#include "CustomComputeBridgeEffect.h"
#include "CustomEffectBase.h"
#include "../Rendering/D2DHelpers.h"

#include <d3dcompiler.h>

namespace ShaderLab::Effects
{
    // ---- Static pass-through pixel shader -------------------------------
    //
    // Compiled once per process and loaded into each D2D device that hosts
    // a bridge. The shader is purely decorative -- the bridge's D2D output
    // is never consumed by downstream graph consumers (they wire to
    // `node->cachedOutput` directly, exactly like the rest of the engine).
    // It exists only so the effect satisfies D2D's "one input -> one
    // output" contract.

    thread_local CustomComputeBridgeEffect* CustomComputeBridgeEffect::s_lastCreated = nullptr;

    namespace
    {
        // {2119663A-884E-4397-8BF8-108659278150}
        constexpr GUID cPassThroughShaderGuid =
        { 0x2119663a, 0x884e, 0x4397, { 0x8b, 0xf8, 0x10, 0x86, 0x59, 0x27, 0x81, 0x50 } };

        // Minimal pass-through pixel shader. D2D pixel shaders take the
        // shape of a free function with the correct signature; the host
        // (D2D) sets up the input texture binding via the effect's
        // transform graph.
        constexpr const char* kPassThroughHLSL = R"HLSL(
Texture2D<float4> Source : register(t0);
SamplerState Sampler   : register(s0);

float4 main(
    float4 clipSpaceOutput : SV_POSITION,
    float4 sceneSpaceOutput : SCENE_POSITION,
    float4 texelSpaceInput0 : TEXCOORD0) : SV_Target
{
    return Source.Sample(Sampler, texelSpaceInput0.xy);
}
)HLSL";

        std::vector<BYTE> CompilePassThrough()
        {
            winrt::com_ptr<ID3DBlob> blob, errors;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            HRESULT hr = D3DCompile(
                kPassThroughHLSL, std::strlen(kPassThroughHLSL),
                "ComputeBridgePassThrough",
                nullptr, nullptr,
                "main", "ps_5_0",
                flags, 0,
                blob.put(), errors.put());
            if (FAILED(hr) || !blob)
            {
                std::string message = std::format("[ShaderLab] Compute bridge pass-through shader failed to compile (0x{:08X})",
                    static_cast<uint32_t>(hr));
                if (errors)
                    message += std::string(": ") + std::string(static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize());
                fprintf(stderr, "%s\n", message.c_str());
                return {};
            }
            std::vector<BYTE> bytecode(blob->GetBufferSize());
            std::memcpy(bytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
            return bytecode;
        }
    }

    // ---- Registration ---------------------------------------------------

    HRESULT CustomComputeBridgeEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        if (!factory) return E_INVALIDARG;

        // One image input. The bridge's shader and cbuffer are set through
        // ICustomComputeBridge, not D2D properties.
        const std::wstring xml = BuildCustomEffectXml(L"Custom Compute Bridge",
            L"D2D wrapper around D3D11 compute shaders for analysis + image output", L"Compute", { L"Source" });
        return factory->RegisterEffectFromString(
            CLSID_CustomComputeBridge, xml.c_str(), nullptr, 0, &CreateFactory);
    }

    HRESULT __stdcall CustomComputeBridgeEffect::CreateFactory(IUnknown** effect)
    {
        if (!effect) return E_POINTER;
        auto* impl = new (std::nothrow) CustomComputeBridgeEffect();
        if (!impl) return E_OUTOFMEMORY;
        s_lastCreated = impl;
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::Create(ID2D1DeviceContext* dc,
        winrt::com_ptr<ID2D1Effect>& outEffect, CustomComputeBridgeEffect** outImpl)
    {
        *outImpl = nullptr;
        s_lastCreated = nullptr;
        const HRESULT hr = dc->CreateEffect(CLSID_CustomComputeBridge, outEffect.put());
        if (SUCCEEDED(hr))
            *outImpl = s_lastCreated;
        s_lastCreated = nullptr;
        return hr;
    }

    // ---- IUnknown -------------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if      (riid == __uuidof(IUnknown))            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1EffectImpl))     *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))  *ppv = static_cast<ID2D1DrawTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))      *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))  *ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == IID_ICustomComputeBridge)      *ppv = static_cast<ICustomComputeBridge*>(this);
        else if (riid == IID_IEngineComputeOutput)      *ppv = static_cast<IEngineComputeOutput*>(this);
        else if (riid == IID_IEngineComputeTexture)     *ppv = static_cast<IEngineComputeTexture*>(this);
        else return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) CustomComputeBridgeEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomComputeBridgeEffect::Release()
    {
        auto count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // ---- ID2D1EffectImpl ------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::PrepareForRender(D2D1_CHANGE_TYPE)
    {
        if (!m_drawInfo) return E_FAIL;
        return EnsurePassThroughShader();
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    HRESULT CustomComputeBridgeEffect::EnsurePassThroughShader()
    {
        // Loaded shaders belong to the D2D device, and a process can hold
        // several, so each effect context is asked.
        if (!m_effectContext->IsShaderLoaded(cPassThroughShaderGuid))
        {
            static const std::vector<BYTE> bytecode = CompilePassThrough();
            if (bytecode.empty())
                return E_FAIL;
            HRESULT hr = m_effectContext->LoadPixelShader(
                cPassThroughShaderGuid, bytecode.data(),
                static_cast<UINT32>(bytecode.size()));
            if (FAILED(hr)) return hr;
        }
        return m_drawInfo->SetPixelShader(cPassThroughShaderGuid);
    }

    // ---- ID2D1DrawTransform ---------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);
        return S_OK;
    }

    // ---- ID2D1Transform -------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount == 0 || !inputRects || !outputRect)
            return E_INVALIDARG;
        // Output rect = input rect (infinite included), or the fixed
        // image-output size. Nothing draws this output: consumers read
        // node->cachedOutput.
        m_inputRect = inputRects[0];
        if (m_imageOutputW > 0 && m_imageOutputH > 0)
        {
            outputRect->left   = 0;
            outputRect->top    = 0;
            outputRect->right  = static_cast<LONG>(m_imageOutputW);
            outputRect->bottom = static_cast<LONG>(m_imageOutputH);
        }
        else
        {
            *outputRect = m_inputRect;
        }
        if (outputOpaqueSubRect)
            *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        if (inputRectCount == 0 || !outputRect || !inputRects)
            return E_INVALIDARG;
        inputRects[0] = *outputRect;
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        if (!invalidOutputRect) return E_INVALIDARG;
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // ---- ICustomComputeBridge -------------------------------------------

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::SetCompiledBytecode(
        const BYTE* bytecode, UINT32 bytecodeSize)
    {
        if (!bytecode || bytecodeSize == 0) return E_INVALIDARG;
        // Content-compare, cheaply: every DXBC blob carries a 16-byte
        // compiler checksum at offset 4. The evaluator calls this before
        // every dispatch with whatever variant the binding plan implies, so
        // it MUST be a no-op when nothing changed, or every GPU-bound
        // consumer recreates its shader twice a frame (variant, then the
        // evaluator's post-dispatch restore of the baseline).
        const ShaderId id = IdOf(bytecode, bytecodeSize);
        const ShaderId& current = m_bytecodeDirty ? m_pendingId : m_installedId;
        if ((m_bytecodeDirty || m_hasInstalled) && id == current)
            return S_OK;
        m_pendingBytecode.assign(bytecode, bytecode + bytecodeSize);
        m_pendingId = id;
        m_bytecodeDirty = true;
        return S_OK;
    }

    const wchar_t* CustomComputeBridgeEffect::DescribeStage(DispatchStage stage)
    {
        switch (stage)
        {
        case DispatchStage::None:         return L"";
        case DispatchStage::Arguments:    return L"checking its inputs";
        case DispatchStage::Device:       return L"finding the D3D11 device";
        case DispatchStage::ShaderCreate: return L"creating the compute shader";
        case DispatchStage::NoShader:     return L"with no shader installed";
        case DispatchStage::InputBounds:  return L"sizing an input";
        case DispatchStage::InputCopy:    return L"copying an input to a texture";
        case DispatchStage::ImageOutput:  return L"creating the output texture";
        case DispatchStage::Run:          return L"running the dispatch";
        }
        return L"";
    }

    HRESULT CustomComputeBridgeEffect::Fail(DispatchStage stage, HRESULT hr)
    {
        m_lastFailure = stage;
        return hr;
    }

    HRESULT CustomComputeBridgeEffect::D3D11DeviceOf(ID2D1DeviceContext5* dc, winrt::com_ptr<ID3D11Device>& device)
    {
        winrt::com_ptr<ID2D1Device> d2dDevice;
        dc->GetDevice(d2dDevice.put());
        auto d2dDevice2 = d2dDevice.try_as<ID2D1Device2>();
        if (!d2dDevice2) return E_NOINTERFACE;
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        if (const HRESULT hr = d2dDevice2->GetDxgiDevice(dxgiDevice.put()); FAILED(hr)) return hr;
        return dxgiDevice->QueryInterface(device.put());
    }

    HRESULT CustomComputeBridgeEffect::InstallPendingShader(ID3D11Device* device)
    {
        if (!m_bytecodeDirty)
            return S_OK;
        // Bytecode that failed once fails again; say so without retrying.
        if (m_hasFailedShader && m_failedShaderId == m_pendingId)
            return m_failedShaderHr;

        // Switching between the baseline and a GPU-binding variant reuses a
        // shader created earlier rather than creating it again.
        winrt::com_ptr<ID3D11ComputeShader> shader;
        for (const auto& v : m_shaderVariants)
            if (v.id == m_pendingId) { shader = v.shader; break; }
        if (!shader)
        {
            const HRESULT hr = device->CreateComputeShader(m_pendingBytecode.data(), m_pendingBytecode.size(),
                                                           nullptr, shader.put());
            if (FAILED(hr))
            {
                m_hasFailedShader = true;
                m_failedShaderId = m_pendingId;
                m_failedShaderHr = hr;
                return hr;
            }
            constexpr size_t cMaxShaderVariants = 4;
            if (m_shaderVariants.size() >= cMaxShaderVariants)
                m_shaderVariants.erase(m_shaderVariants.begin());
            m_shaderVariants.push_back({ m_pendingId, shader });
        }
        m_runner.InstallPrecompiledShader(m_pendingBytecode, shader);
        m_installedId = m_pendingId;
        m_hasInstalled = true;
        m_bytecodeDirty = false;
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::PrepareInputTexture(ID2D1DeviceContext5* dc, ID2D1Image* image,
                                                           UINT32 slot, winrt::com_ptr<ID3D11Texture2D>& texture)
    {
        D2D1_RECT_L px{};
        if (const HRESULT hr = Rendering::GetSnappedPixelBounds(dc, image, px); FAILED(hr))
            return Fail(DispatchStage::InputBounds, hr);
        const UINT32 w = static_cast<UINT32>(px.right - px.left);
        const UINT32 h = static_cast<UINT32>(px.bottom - px.top);

        // A bitmap the shader can read in place -- the evaluator's shared
        // pre-render, a source's frame, an upstream compute node's output:
        // bind its texture directly.
        if (texture = Rendering::ComputeReadableTexture(image, px); texture)
            return S_OK;

        auto& cache = m_inputBitmaps[slot];
        if (!cache.bitmap || cache.width != w || cache.height != h)
        {
            D2D1_BITMAP_PROPERTIES1 bp{};
            bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            bp.dpiX = 96.0f;
            bp.dpiY = 96.0f;
            winrt::com_ptr<ID2D1Bitmap1> created;
            if (const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, created.put()); FAILED(hr))
                return Fail(DispatchStage::InputCopy, hr);
            cache = { std::move(created), w, h };
        }
        // Draw the upstream chain into it in a session of its own; DrawImage
        // does the format conversion, and EndDraw submits it (D2D batches
        // until then).
        HRESULT drawHr = S_OK;
        {
            Rendering::ScopedTarget target(dc, cache.bitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawImage(image, D2D1::Point2F(-static_cast<float>(px.left), -static_cast<float>(px.top)));
            drawHr = dc->EndDraw();
        }
        if (FAILED(drawHr))
            return Fail(DispatchStage::InputCopy, drawHr);

        winrt::com_ptr<IDXGISurface> surface;
        if (const HRESULT hr = cache.bitmap->GetSurface(surface.put()); FAILED(hr))
            return Fail(DispatchStage::InputCopy, hr);
        if (const HRESULT hr = surface->QueryInterface(texture.put()); FAILED(hr))
            return Fail(DispatchStage::InputCopy, hr);
        return S_OK;
    }

    Rendering::D3D11ComputeRunner::Readback CustomComputeBridgeEffect::ChooseReadback(bool wantCpu) const
    {
        if (!wantCpu) return Rendering::D3D11ComputeRunner::Readback::None;
        return m_asyncReadback ? Rendering::D3D11ComputeRunner::Readback::Async
                               : Rendering::D3D11ComputeRunner::Readback::Blocking;
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::Dispatch(
        ID2D1DeviceContext5* dc,
        ID2D1Image* const* inputImages,
        UINT32 inputCount,
        const BYTE* cbufferData,
        UINT32 cbufferSize,
        UINT32 analysisFloat4Count,
        UINT32 imageOutputW,
        UINT32 imageOutputH,
        std::vector<float>* outAnalysisFloats)
    {
        // Bindings, dims and the readback mode apply to this call only, on
        // every exit path: the bindings are raw SRVs the caller owns.
        struct ResetPerCallState
        {
            CustomComputeBridgeEffect* self;
            ~ResetPerCallState()
            {
                self->m_asyncReadback = false;
                self->m_gpuBindingSrvs.clear();
                self->m_gpuBindingSlots.clear();
                self->m_dispatchX = self->m_dispatchY = self->m_dispatchZ = 1;
            }
        } resetPerCallState{ this };
        m_lastFailure = DispatchStage::None;

        // 1. Arguments.
        if (!dc || !inputImages || inputCount == 0)
            return Fail(DispatchStage::Arguments, E_INVALIDARG);
        for (UINT32 slot = 0; slot < inputCount; ++slot)
            if (!inputImages[slot]) return Fail(DispatchStage::Arguments, E_INVALIDARG);

        // 2. The shader, before the inputs: it is the cheap step, and a
        // shader that cannot be created must not cost an input copy a frame.
        winrt::com_ptr<ID3D11Device> device;
        if (const HRESULT hr = D3D11DeviceOf(dc, device); FAILED(hr))
            return Fail(DispatchStage::Device, hr);
        if (!m_runner.IsInitialized())
            m_runner.Initialize(device.get());
        if (const HRESULT hr = InstallPendingShader(device.get()); FAILED(hr))
            return Fail(DispatchStage::ShaderCreate, hr);
        if (!m_runner.HasShader())
            return Fail(DispatchStage::NoShader, E_NOT_VALID_STATE);

        // 3. Each input as an FP32 texture; 96 DPI keeps bounds in pixels.
        std::vector<winrt::com_ptr<ID3D11Texture2D>> inputTextures(inputCount);
        {
            Rendering::ScopedDpi96 dpi(dc);
            if (m_inputBitmaps.size() < inputCount)
                m_inputBitmaps.resize(inputCount);
            for (UINT32 slot = 0; slot < inputCount; ++slot)
                if (const HRESULT hr = PrepareInputTexture(dc, inputImages[slot], slot, inputTextures[slot]); FAILED(hr))
                    return hr;
        }

        // 4. The image-output texture, when one is requested.
        if (imageOutputW > 0 && imageOutputH > 0)
        {
            if (const HRESULT hr = EnsureImageOutputTexture(device.get(), dc, imageOutputW, imageOutputH); FAILED(hr))
                return Fail(DispatchStage::ImageOutput, hr);
        }
        else if (m_imageOutputTex)
        {
            m_imageOutput = nullptr;
            m_imageOutputTex = nullptr;
            m_imageOutputW = 0;
            m_imageOutputH = 0;
        }

        // 5. Dispatch. The runner prepends Width/Height to the cbuffer, and
        // records the whole dispatch on its own deferred context (see
        // D3D11ComputeRunner::Initialize), so no lock is taken here. A null
        // outAnalysisFloats skips the CopyResource + Map.
        std::vector<BYTE> cbBytes;
        if (cbufferData && cbufferSize > 0)
            cbBytes.assign(cbufferData, cbufferData + cbufferSize);
        std::vector<ID3D11Texture2D*> inputRaw;
        inputRaw.reserve(inputTextures.size());
        for (const auto& t : inputTextures) inputRaw.push_back(t.get());

        auto floats = m_runner.DispatchWithImageOutput(
            inputRaw,
            cbBytes,
            analysisFloat4Count,
            m_imageOutputTex.get(),
            m_dispatchX, m_dispatchY, m_dispatchZ,
            m_gpuBindingSrvs, m_gpuBindingSlots,
            ChooseReadback(outAnalysisFloats != nullptr));

        if (outAnalysisFloats)
            *outAnalysisFloats = std::move(floats);

        if (FAILED(m_runner.LastDispatchResult()))
            return Fail(DispatchStage::Run, m_runner.LastDispatchResult());

        m_lastEvaluatedFrame++;
        return S_OK;
    }

    CustomComputeBridgeEffect::ShaderId CustomComputeBridgeEffect::IdOf(
        const BYTE* bytes, size_t size)
    {
        ShaderId id{};
        if (size >= 20 && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C')
        {
            std::memcpy(id.data(), bytes + 4, id.size());
            return id;
        }
        // Not DXBC (never expected): fall back to a full FNV-1a so two
        // different blobs still compare different.
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
        std::memcpy(id.data(), &h, sizeof(h));
        const uint64_t sz = size;
        std::memcpy(id.data() + 8, &sz, sizeof(sz));
        return id;
    }

    bool CustomComputeBridgeEffect::PollAnalysisReadback(std::vector<float>& out)
    {
        return m_runner.PollReadback(out);
    }

    HRESULT CustomComputeBridgeEffect::SetGpuBinding(
        UINT32 slot, ID3D11ShaderResourceView* srv)
    {
        // Cleared automatically at end of each Dispatch; caller binds
        // fresh per frame from the evaluator's binding-resolution pass.
        m_gpuBindingSrvs.push_back(srv);
        m_gpuBindingSlots.push_back(slot);
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::SetDispatchDims(
        UINT32 x, UINT32 y, UINT32 z)
    {
        m_dispatchX = x;
        m_dispatchY = y;
        m_dispatchZ = z;
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::EnsureImageOutputTexture(
        ID3D11Device* device,
        ID2D1DeviceContext5* dc,
        UINT32 w, UINT32 h)
    {
        if (!device || !dc) return E_INVALIDARG;
        if (m_imageOutputTex && m_imageOutputW == w && m_imageOutputH == h)
            return S_OK;

        m_imageOutput = nullptr;
        m_imageOutputTex = nullptr;
        m_imageOutputW = w;
        m_imageOutputH = h;

        D3D11_TEXTURE2D_DESC td{};
        td.Width  = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, m_imageOutputTex.put());
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IDXGISurface> surface;
        hr = m_imageOutputTex->QueryInterface(surface.put());
        if (FAILED(hr)) return hr;

        // Wrap as ID2D1Bitmap1. DPI must be 96 -- decision-log gotcha:
        // default 0 DPI causes GetImageLocalBounds to return zeros.
        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        bp.dpiX = 96.0f;
        bp.dpiY = 96.0f;
        return dc->CreateBitmapFromDxgiSurface(surface.get(), &bp, m_imageOutput.put());
    }

    // ---- IEngineComputeOutput -------------------------------------------

    // ---- IEngineComputeTexture (lane 3) -----------------------------------
    // Straight delegation: the runner owns the texture, the copy shader and
    // the decision not to create either until asked. Requesting the lane is
    // what makes a compute node pay for it, so the request has to reach the
    // runner rather than being cached here.

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::RequestAnalysisTexture()
    {
        return m_runner.RequestAnalysisTexture();
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisTexture(
        ID3D11Texture2D** out)
    {
        return m_runner.GetAnalysisTexture(out);
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisTextureSrv(
        ID3D11ShaderResourceView** out)
    {
        return m_runner.GetAnalysisTextureSrv(out);
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisSrv(
        ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        // Delegate to the runner. Returns E_NOT_VALID_STATE before the
        // first successful Dispatch (the runner's SRV is null until the
        // result buffer has been allocated).
        return m_runner.GetAnalysisSrv(out);
    }
}
