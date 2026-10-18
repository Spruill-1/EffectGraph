#include "pch_engine.h"
#include "VideoFrameConverter.h"

namespace ShaderLab::Effects
{
    namespace
    {
        // Video frame -> linear scRGB FP16. Compiled per stream; the SL_*
        // macros come from the stream's format and colour description.
        //   SL_INPUT_RGB    1: BGRA8 from the video processor (sRGB curve)
        //   SL_CODE_MAX     the largest code value (255 or 1023)
        //   SL_Y_BLACK / SL_Y_WHITE, SL_C_MIN / SL_C_MID / SL_C_MAX: range
        //   SL_R_CR, SL_G_CB, SL_G_CR, SL_B_CB: YCbCr -> RGB coefficients
        //   SL_TRANSFER     0 sRGB curve, 1 PQ, 2 HLG
        //   SL_GAMUT_2020   1: BT.2020 primaries, converted to BT.709
        constexpr const char* cConvertTemplate = R"(
#if SL_INPUT_RGB
Texture2D<float4> texRGB : register(t0);
#else
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
#endif
RWTexture2D<float4> output : register(u0);

cbuffer Params : register(b0) { uint Width; uint Height; uint Pad0; uint Pad1; };

float SRGBToLinear(float s)
{
    return (s <= 0.04045) ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

float InversePQ(float N)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float Np = pow(max(N, 0.0), 1.0 / m2);
    float num = max(Np - c1, 0.0);
    float den = c2 - c3 * Np;
    return (den <= 0.0) ? 0.0 : 10000.0 * pow(num / den, 1.0 / m1);
}

// BT.2100 HLG inverse OETF: signal -> normalized scene light.
float InverseHLG(float E)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    return (E <= 0.5) ? (E * E) / 3.0 : (exp((E - c) / a) + b) / 12.0;
}

float3 Bt2020To709(float3 n)
{
    float3 rgb709;
    rgb709.r =  1.6605 * n.r - 0.5877 * n.g - 0.0728 * n.b;
    rgb709.g = -0.1246 * n.r + 1.1330 * n.g - 0.0084 * n.b;
    rgb709.b = -0.0182 * n.r - 0.1006 * n.g + 1.1187 * n.b;
    return rgb709;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

#if SL_INPUT_RGB
    float4 px = texRGB[id.xy]; // BGRA as float4 via B8G8R8A8_UNORM SRV
    output[id.xy] = float4(SRGBToLinear(px.r), SRGBToLinear(px.g), SRGBToLinear(px.b), 1.0);
#else
    float yVal = texY[id.xy];
    float2 uv = texUV[uint2(id.x / 2, id.y / 2)];

    float yScaled = (yVal * SL_CODE_MAX - SL_Y_BLACK) / (SL_Y_WHITE - SL_Y_BLACK);
    float cb = (uv.x * SL_CODE_MAX - SL_C_MID) / (SL_C_MAX - SL_C_MIN);
    float cr = (uv.y * SL_CODE_MAX - SL_C_MID) / (SL_C_MAX - SL_C_MIN);
    yScaled = max(yScaled, 0.0);

    float r = saturate(yScaled + SL_R_CR * cr);
    float g = saturate(yScaled - SL_G_CB * cb - SL_G_CR * cr);
    float b = saturate(yScaled + SL_B_CB * cb);

#if SL_TRANSFER == 1
    // PQ -> nits -> scRGB (80 nits = 1.0).
    float nR = InversePQ(r);
    float nG = InversePQ(g);
    float nB = InversePQ(b);
#if SL_GAMUT_2020
    float3 rgb709 = Bt2020To709(float3(nR, nG, nB));
#else
    float3 rgb709 = float3(nR, nG, nB);
#endif
    output[id.xy] = float4(rgb709 / 80.0, 1.0);
#elif SL_TRANSFER == 2
    // HLG -> scene light -> display nits (1000-nit OOTF, system gamma 1.2) -> scRGB.
    float3 scene = float3(InverseHLG(r), InverseHLG(g), InverseHLG(b));
    float ys = dot(scene, float3(0.2627, 0.6780, 0.0593));
    float3 nits = 1000.0 * pow(max(ys, 1e-6), 0.2) * scene;
#if SL_GAMUT_2020
    nits = Bt2020To709(nits);
#endif
    output[id.xy] = float4(nits / 80.0, 1.0);
#else
    float3 lin = float3(SRGBToLinear(r), SRGBToLinear(g), SRGBToLinear(b));
#if SL_GAMUT_2020
    lin = Bt2020To709(lin);
#endif
    output[id.xy] = float4(lin, 1.0);
#endif
#endif
}
)";

        struct RangeLiterals { const char* codeMax; const char* yBlack; const char* yWhite; const char* cMin; const char* cMid; const char* cMax; };
        struct MatrixLiterals { const char* rCr; const char* gCb; const char* gCr; const char* bCb; };

        RangeLiterals RangeFor(bool tenBit, VideoColorDescription::Range range)
        {
            const bool full = range == VideoColorDescription::Range::Full;
            if (tenBit)
                return full ? RangeLiterals{ "1023.0", "0.0", "1023.0", "0.0", "512.0", "1023.0" }
                            : RangeLiterals{ "1023.0", "64.0", "940.0", "64.0", "512.0", "960.0" };
            return full ? RangeLiterals{ "255.0", "0.0", "255.0", "0.0", "128.0", "255.0" }
                        : RangeLiterals{ "255.0", "16.0", "235.0", "16.0", "128.0", "240.0" };
        }

        MatrixLiterals MatrixFor(VideoColorDescription::Matrix matrix)
        {
            switch (matrix)
            {
            case VideoColorDescription::Matrix::Bt601:  return { "1.4020", "0.3441", "0.7141", "1.7720" };
            case VideoColorDescription::Matrix::Bt2020: return { "1.47460", "0.16455", "0.57135", "1.88100" };
            default:                                     return { "1.5748", "0.1873", "0.4681", "1.8556" };
            }
        }
    }

    std::string VideoFrameConverter::ShaderSource(VideoFrameFormat format, const VideoColorDescription& color)
    {
        const bool rgb = format == VideoFrameFormat::RGB32;
        const RangeLiterals range = RangeFor(format == VideoFrameFormat::P010, color.range);
        const MatrixLiterals matrix = MatrixFor(color.matrix);
        int transfer = 0;
        if (!rgb && color.transfer == VideoColorDescription::Transfer::Pq) transfer = 1;
        if (!rgb && color.transfer == VideoColorDescription::Transfer::Hlg) transfer = 2;
        const bool gamut2020 = !rgb && color.primaries == VideoColorDescription::Primaries::Bt2020;

        std::string source;
        source += std::format("#define SL_INPUT_RGB {}\n", rgb ? 1 : 0);
        source += std::format("#define SL_CODE_MAX {}\n#define SL_Y_BLACK {}\n#define SL_Y_WHITE {}\n",
                              range.codeMax, range.yBlack, range.yWhite);
        source += std::format("#define SL_C_MIN {}\n#define SL_C_MID {}\n#define SL_C_MAX {}\n",
                              range.cMin, range.cMid, range.cMax);
        source += std::format("#define SL_R_CR {}\n#define SL_G_CB {}\n#define SL_G_CR {}\n#define SL_B_CB {}\n",
                              matrix.rCr, matrix.gCb, matrix.gCr, matrix.bCb);
        source += std::format("#define SL_TRANSFER {}\n#define SL_GAMUT_2020 {}\n", transfer, gamut2020 ? 1 : 0);
        source += cConvertTemplate;
        return source;
    }

    bool VideoFrameConverter::Create(ID3D11Device* device, ID3D11DeviceContext* immediateContext,
                                     VideoFrameFormat format, const VideoColorDescription& color,
                                     uint32_t width, uint32_t height)
    {
        Reset();
        m_device = device;
        m_immediate = immediateContext;
        m_format = format;
        m_color = color;
        m_width = width;
        m_height = height;
        if (!device || !immediateContext || width == 0 || height == 0)
        {
            m_lastError = L"Failed to create GPU conversion resources: no device or an empty frame";
            return false;
        }
        if (FAILED(device->CreateDeferredContext(0, m_convertCtx.put())))
            m_convertCtx = nullptr;   // falls back to the immediate context

        if (!CreateResources())
        {
            m_lastError = L"Failed to create GPU conversion resources: " + m_lastError;
            return false;
        }
        if (!CompileShader())
        {
            m_lastError = L"Failed to compile conversion shader: " + m_lastError;
            return false;
        }
        return true;
    }

    void VideoFrameConverter::Reset()
    {
        m_texPlanar = nullptr; m_srvPlanarY = nullptr; m_srvPlanarUV = nullptr;
        m_shader = nullptr;
        m_texY = nullptr; m_texUV = nullptr; m_texRGB = nullptr;
        m_texOutput = nullptr;
        m_srvY = nullptr; m_srvUV = nullptr; m_srvRGB = nullptr;
        m_uavOutput = nullptr; m_cbParams = nullptr;
        m_convertCtx = nullptr;
        m_device = nullptr; m_immediate = nullptr;
        m_width = 0; m_height = 0;
    }

    bool VideoFrameConverter::CreateResources()
    {
        auto* device = m_device;
        HRESULT hr;

        // Output: R16G16B16A16_FLOAT, written by the shader and read by D2D.
        D3D11_TEXTURE2D_DESC outDesc{};
        outDesc.Width = m_width;
        outDesc.Height = m_height;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&outDesc, nullptr, m_texOutput.put());
        if (FAILED(hr)) { m_lastError = L"CreateTexture2D output"; return false; }

        hr = device->CreateUnorderedAccessView(m_texOutput.get(), nullptr, m_uavOutput.put());
        if (FAILED(hr)) { m_lastError = L"CreateUAV output"; return false; }

        // Upload textures for CPU-copied frames.
        auto createPlane = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                               winrt::com_ptr<ID3D11Texture2D>& tex, winrt::com_ptr<ID3D11ShaderResourceView>& srv,
                               const wchar_t* texError, const wchar_t* srvError)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w; desc.Height = h;
            desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = fmt;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put()))) { m_lastError = texError; return false; }
            if (FAILED(device->CreateShaderResourceView(tex.get(), nullptr, srv.put()))) { m_lastError = srvError; return false; }
            return true;
        };
        if (m_format == VideoFrameFormat::P010)
        {
            if (!createPlane(m_width, m_height, DXGI_FORMAT_R16_UNORM, m_texY, m_srvY, L"CreateTexture2D Y (P010)", L"CreateSRV Y") ||
                !createPlane(m_width / 2, m_height / 2, DXGI_FORMAT_R16G16_UNORM, m_texUV, m_srvUV, L"CreateTexture2D UV (P010)", L"CreateSRV UV"))
                return false;
        }
        else if (m_format == VideoFrameFormat::NV12)
        {
            if (!createPlane(m_width, m_height, DXGI_FORMAT_R8_UNORM, m_texY, m_srvY, L"CreateTexture2D Y (NV12)", L"CreateSRV Y") ||
                !createPlane(m_width / 2, m_height / 2, DXGI_FORMAT_R8G8_UNORM, m_texUV, m_srvUV, L"CreateTexture2D UV (NV12)", L"CreateSRV UV"))
                return false;
        }
        else if (!createPlane(m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, m_texRGB, m_srvRGB, L"CreateTexture2D RGB", L"CreateSRV RGB"))
        {
            return false;
        }

        struct { uint32_t w, h, pad0, pad1; } cbData = { m_width, m_height, 0, 0 };
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(cbData);
        cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbInit{ &cbData, 0, 0 };
        hr = device->CreateBuffer(&cbDesc, &cbInit, m_cbParams.put());
        if (FAILED(hr)) { m_lastError = L"CreateBuffer cbParams"; return false; }

        return true;
    }

    bool VideoFrameConverter::CompileShader()
    {
        const std::string source = ShaderSource(m_format, m_color);
        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(source.data(), source.size(), "VideoConvert",
            nullptr, nullptr, "main", "cs_5_0", 0, 0, blob.put(), errors.put());
        if (FAILED(hr))
        {
            if (errors)
                m_lastError = std::wstring(L"Shader compile: ") +
                    std::wstring(reinterpret_cast<const char*>(errors->GetBufferPointer()),
                                 reinterpret_cast<const char*>(errors->GetBufferPointer()) + errors->GetBufferSize());
            else
                m_lastError = std::format(L"D3DCompile failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_shader.put());
        if (FAILED(hr)) { m_lastError = L"CreateComputeShader"; return false; }
        return true;
    }

    bool VideoFrameConverter::CanConvertSurface(ID3D11Texture2D* decoderTexture)
    {
        if (m_texPlanar) return true;
        if (!decoderTexture || !m_device) return false;
        D3D11_TEXTURE2D_DESC sd{};
        decoderTexture->GetDesc(&sd);
        DXGI_FORMAT lumaFmt, chromaFmt;
        if (sd.Format == DXGI_FORMAT_NV12)      { lumaFmt = DXGI_FORMAT_R8_UNORM;  chromaFmt = DXGI_FORMAT_R8G8_UNORM; }
        else if (sd.Format == DXGI_FORMAT_P010) { lumaFmt = DXGI_FORMAT_R16_UNORM; chromaFmt = DXGI_FORMAT_R16G16_UNORM; }
        else return false;
        // The shader was compiled for the reader's output subtype, so the
        // decoder surface has to match it.
        if ((sd.Format == DXGI_FORMAT_NV12) != (m_format == VideoFrameFormat::NV12)) return false;

        UINT support = 0;
        if (FAILED(m_device->CheckFormatSupport(sd.Format, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ||
            !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
            return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_width;
        td.Height = m_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, tex.put()))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        winrt::com_ptr<ID3D11ShaderResourceView> srvY, srvUV;
        vd.Format = lumaFmt;
        if (FAILED(m_device->CreateShaderResourceView(tex.get(), &vd, srvY.put()))) return false;
        vd.Format = chromaFmt;
        if (FAILED(m_device->CreateShaderResourceView(tex.get(), &vd, srvUV.put()))) return false;

        m_texPlanar = std::move(tex);
        m_srvPlanarY = std::move(srvY);
        m_srvPlanarUV = std::move(srvUV);
        return true;
    }

    bool VideoFrameConverter::ConvertBytes(const BYTE* data, UINT pitch)
    {
        if (!m_immediate || !data) return false;
        // Single immediate-context calls, ordered before the conversion.
        if (m_format == VideoFrameFormat::P010 || m_format == VideoFrameFormat::NV12)
        {
            D3D11_BOX yBox = { 0, 0, 0, m_width, m_height, 1 };
            m_immediate->UpdateSubresource(m_texY.get(), 0, &yBox, data, pitch, 0);
            const BYTE* uvData = data + static_cast<ptrdiff_t>(pitch) * m_height;
            D3D11_BOX uvBox = { 0, 0, 0, m_width / 2, m_height / 2, 1 };
            m_immediate->UpdateSubresource(m_texUV.get(), 0, &uvBox, uvData, pitch, 0);
        }
        else
        {
            D3D11_BOX box = { 0, 0, 0, m_width, m_height, 1 };
            m_immediate->UpdateSubresource(m_texRGB.get(), 0, &box, data, pitch, 0);
        }
        return Run(false, nullptr, 0);
    }

    bool VideoFrameConverter::ConvertSurface(ID3D11Texture2D* decoderTexture, UINT subresource)
    {
        if (!m_texPlanar || !decoderTexture) return false;
        return Run(true, decoderTexture, subresource);
    }

    bool VideoFrameConverter::Run(bool planar, ID3D11Texture2D* decoderTex, UINT decoderSubresource)
    {
        if (!m_shader)
        {
            m_lastError = L"No conversion shader for the output format";
            return false;
        }

        ID3D11DeviceContext* context = m_convertCtx ? m_convertCtx.get() : m_immediate;

        // Zero-copy: copy both planes of the decoder surface into the planar
        // texture, recorded with the conversion so the two run together.
        if (decoderTex)
        {
            D3D11_BOX box = { 0, 0, 0, m_width, m_height, 1 };
            context->CopySubresourceRegion(m_texPlanar.get(), 0, 0, 0, 0,
                decoderTex, decoderSubresource, &box);
        }

        context->CSSetShader(m_shader.get(), nullptr, 0);

        ID3D11Buffer* cbs[] = { m_cbParams.get() };
        context->CSSetConstantBuffers(0, 1, cbs);

        if (m_format == VideoFrameFormat::RGB32)
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvRGB.get() };
            context->CSSetShaderResources(0, 1, srvs);
        }
        else if (planar)
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvPlanarY.get(), m_srvPlanarUV.get() };
            context->CSSetShaderResources(0, 2, srvs);
        }
        else
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvY.get(), m_srvUV.get() };
            context->CSSetShaderResources(0, 2, srvs);
        }

        ID3D11UnorderedAccessView* uavs[] = { m_uavOutput.get() };
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // 16x16 thread groups.
        UINT gx = (m_width + 15) / 16;
        UINT gy = (m_height + 15) / 16;
        context->Dispatch(gx, gy, 1);

        ID3D11ShaderResourceView* nullSRVs[2] = {};
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        context->CSSetShaderResources(0, 2, nullSRVs);
        context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);

        if (m_convertCtx)
        {
            winrt::com_ptr<ID3D11CommandList> commandList;
            const HRESULT hr = m_convertCtx->FinishCommandList(FALSE, commandList.put());
            if (FAILED(hr) || !commandList)
            {
                m_lastError = std::format(L"FinishCommandList failed: 0x{:08X}", static_cast<uint32_t>(hr));
                return false;
            }
            // TRUE keeps the immediate context's state, which D2D on
            // another thread may be in the middle of setting.
            m_immediate->ExecuteCommandList(commandList.get(), TRUE);
        }
        return true;
    }
}
