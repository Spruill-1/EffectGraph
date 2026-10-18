#include "pch_engine.h"
#include "SourceTextureUtil.h"

namespace ShaderLab::Effects
{
    std::wstring HResultMessage(HRESULT hr)
    {
        const auto code = static_cast<unsigned long>(hr);
        return L"HRESULT " + std::format(L"0x{:08X}", code);
    }

    bool IsLinearScRgbFormat(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    winrt::com_ptr<ID2D1Image> AsLinearScRgb(ID2D1DeviceContext5* dc, ID2D1Bitmap1* bitmap,
                                             DXGI_FORMAT format,
                                             winrt::com_ptr<ID2D1Effect>& decode,
                                             std::wstring& error)
    {
        decode = nullptr;
        if (!dc || !bitmap)
        {
            error = L"No device context or bitmap to decode";
            return nullptr;
        }
        winrt::com_ptr<ID2D1Image> image;
        if (IsLinearScRgbFormat(format))
        {
            image.copy_from(bitmap);
            return image;
        }

        // sRGB to scRGB through ColorManagement, at float precision, as
        // ImageLoader decodes 8-bit files.
        winrt::com_ptr<ID2D1ColorContext> srgb, scrgb;
        HRESULT hr = dc->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, srgb.put());
        if (SUCCEEDED(hr))
            hr = dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, scrgb.put());
        if (SUCCEEDED(hr))
            hr = dc->CreateEffect(CLSID_D2D1ColorManagement, decode.put());
        if (FAILED(hr) || !decode)
        {
            error = L"Failed to create the sRGB-to-linear decode: " + HResultMessage(hr);
            decode = nullptr;
            return nullptr;
        }
        decode->SetInput(0, bitmap);
        decode->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srgb.get());
        decode->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, scrgb.get());
        decode->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);
        decode->GetOutput(image.put());
        return image;
    }

    SharedOutput CreateSharedOutput(ID2D1DeviceContext5* dc, ID3D11Device* device,
                                    uint32_t width, uint32_t height, DXGI_FORMAT format,
                                    std::wstring_view what)
    {
        SharedOutput output;
        if (!dc || !device || width == 0 || height == 0)
        {
            output.error = std::format(L"No device or an empty size for the {} output", what);
            return output;
        }

        // One mip, one slice: D2D draws into it and the graph samples it.
        const CD3D11_TEXTURE2D_DESC desc(format, width, height, 1, 1,
                                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, output.texture.put());
        if (FAILED(hr) || !output.texture)
        {
            output.Reset();
            output.error = std::format(L"Failed to create {} output texture: {}", what, HResultMessage(hr));
            return output;
        }

        winrt::com_ptr<IDXGISurface> surface;
        hr = output.texture->QueryInterface(IID_PPV_ARGS(surface.put()));
        if (FAILED(hr) || !surface)
        {
            output.Reset();
            output.error = std::format(L"Failed to query {} output texture surface: {}", what, HResultMessage(hr));
            return output;
        }

        const D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &bitmapProps, output.bitmap.put());
        if (FAILED(hr) || !output.bitmap)
        {
            output.Reset();
            output.error = std::format(L"Failed to create D2D bitmap for {} output: {}", what, HResultMessage(hr));
            return output;
        }

        std::wstring decodeError;
        output.image = AsLinearScRgb(dc, output.bitmap.get(), format, output.decode, decodeError);
        if (!output.image)
        {
            output.Reset();
            output.error = std::format(L"Failed to decode the {} output to linear: {}", what, decodeError);
        }
        return output;
    }
}
