#include "pch_engine.h"
#include "ImageCapture.h"
#include "../Graph/GraphModel.h"

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace ShaderLab::Rendering
{
    namespace
    {
        // Restores a context's target, DPI and transform on scope exit.
        class ContextStateGuard
        {
        public:
            explicit ContextStateGuard(ID2D1DeviceContext* dc) : m_dc(dc)
            {
                m_dc->GetTarget(m_target.put());
                m_dc->GetDpi(&m_dpiX, &m_dpiY);
                m_dc->GetTransform(&m_transform);
            }
            ~ContextStateGuard()
            {
                m_dc->SetTarget(m_target.get());
                m_dc->SetDpi(m_dpiX, m_dpiY);
                m_dc->SetTransform(m_transform);
            }
            ContextStateGuard(const ContextStateGuard&) = delete;
            ContextStateGuard& operator=(const ContextStateGuard&) = delete;

        private:
            ID2D1DeviceContext* m_dc;
            winrt::com_ptr<ID2D1Image> m_target;
            float m_dpiX{ 96.0f };
            float m_dpiY{ 96.0f };
            D2D1_MATRIX_3X2_F m_transform{};
        };

        HRESULT WriteFrame(IWICImagingFactory* wic, IWICBitmapEncoder* encoder, const PixelBuffer& pixels)
        {
            const bool half = pixels.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2> options;
            HRESULT hr = encoder->CreateNewFrame(frame.put(), options.put());
            if (FAILED(hr)) return hr;
            if (half && options)
            {
                // Lossless: the HDR path exists for fidelity, not size.
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"Lossless");
                VARIANT value{};
                value.vt = VT_BOOL;
                value.boolVal = VARIANT_TRUE;
                hr = options->Write(1, &option, &value);
                if (FAILED(hr)) return hr;
            }
            hr = frame->Initialize(options.get());
            if (FAILED(hr)) return hr;
            hr = frame->SetSize(pixels.width, pixels.height);
            if (FAILED(hr)) return hr;
            WICPixelFormatGUID format = half ? GUID_WICPixelFormat64bppRGBAHalf : GUID_WICPixelFormat32bppBGRA;
            const WICPixelFormatGUID wanted = format;
            hr = frame->SetPixelFormat(&format);
            if (FAILED(hr)) return hr;
            // The encoder may substitute a format; refuse rather than write bytes it misreads.
            if (format != wanted) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
            if (half)
            {
                // Tags the linear Rec.709 primaries; a reader without it still
                // treats 64bpp half as scRGB, so a failure here is not fatal.
                winrt::com_ptr<IWICColorContext> colorContext;
                if (SUCCEEDED(wic->CreateColorContext(colorContext.put())) &&
                    SUCCEEDED(colorContext->InitializeFromExifColorSpace(1)))
                {
                    IWICColorContext* contexts[] = { colorContext.get() };
                    frame->SetColorContexts(1, contexts);
                }
            }
            hr = frame->WritePixels(pixels.height, pixels.pitch, static_cast<UINT>(pixels.bytes.size()),
                                    const_cast<BYTE*>(pixels.bytes.data()));
            if (FAILED(hr)) return hr;
            hr = frame->Commit();
            if (FAILED(hr)) return hr;
            return encoder->Commit();
        }

        bool IsBgra8(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
    }

    ID2D1Image* ResolveNodeImage(const Graph::GraphModel& graph, uint32_t nodeId, NodeImageStatus* status)
    {
        auto report = [&](NodeImageStatus value) { if (status) *status = value; };
        const auto* node = graph.FindNode(nodeId);
        if (!node) { report(NodeImageStatus::NotFound); return nullptr; }
        // A dirty node's cachedOutput may already be released by the evaluator.
        if (!node->cachedOutput || node->dirty) { report(NodeImageStatus::NotReady); return nullptr; }
        if (!node->inputPins.empty() && graph.GetInputEdges(nodeId).empty())
        {
            report(NodeImageStatus::NotReady);
            return nullptr;
        }
        report(NodeImageStatus::Ready);
        return node->cachedOutput;
    }

    HRESULT GetImageBounds96Dpi(ID2D1DeviceContext* dc, ID2D1Image* image, D2D1_RECT_F& bounds)
    {
        if (!dc || !image) return E_POINTER;
        float dpiX = 96.0f, dpiY = 96.0f;
        dc->GetDpi(&dpiX, &dpiY);
        dc->SetDpi(96.0f, 96.0f);
        const HRESULT hr = dc->GetImageLocalBounds(image, &bounds);
        dc->SetDpi(dpiX, dpiY);
        return hr;
    }

    HRESULT RenderToPixels(ID2D1DeviceContext* dc, uint32_t width, uint32_t height,
                           DXGI_FORMAT format, const D2D1_COLOR_F& clear,
                           const std::function<void(ID2D1DeviceContext*)>& draw,
                           PixelBuffer& out)
    {
        out = {};
        if (!dc) return E_POINTER;
        if (width == 0 || height == 0) return E_INVALIDARG;

        const D2D1_PIXEL_FORMAT pixelFormat = D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED);
        winrt::com_ptr<ID2D1Bitmap1> target;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixelFormat, 96.0f, 96.0f), target.put());
        if (FAILED(hr)) return hr;

        {
            ContextStateGuard restore(dc);
            dc->SetTarget(target.get());
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->BeginDraw();
            dc->Clear(clear);
            draw(dc);
            hr = dc->EndDraw();
        }
        if (FAILED(hr)) return hr;

        winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
        hr = dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    pixelFormat, 96.0f, 96.0f),
            cpuBitmap.put());
        if (FAILED(hr)) return hr;
        const D2D1_POINT_2U origin{ 0, 0 };
        const D2D1_RECT_U rect{ 0, 0, width, height };
        hr = cpuBitmap->CopyFromBitmap(&origin, target.get(), &rect);
        if (FAILED(hr)) return hr;

        D2D1_MAPPED_RECT mapped{};
        hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return hr;
        out.width  = width;
        out.height = height;
        out.pitch  = mapped.pitch;
        out.format = format;
        out.bytes.assign(mapped.bits, mapped.bits + static_cast<size_t>(mapped.pitch) * height);
        cpuBitmap->Unmap();
        return S_OK;
    }

    HRESULT EncodePng(const PixelBuffer& pixels, std::vector<uint8_t>& png)
    {
        png.clear();
        if (!IsBgra8(pixels.format)) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        winrt::com_ptr<IWICImagingFactory> wic;
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        if (FAILED(hr)) return hr;
        winrt::com_ptr<IStream> stream;
        hr = ::CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
        if (FAILED(hr)) return hr;
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
        if (FAILED(hr)) return hr;
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return hr;
        hr = WriteFrame(wic.get(), encoder.get(), pixels);
        if (FAILED(hr)) return hr;

        STATSTG stat{};
        hr = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(hr)) return hr;
        png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        const LARGE_INTEGER start{};
        hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) return hr;
        ULONG read = 0;
        hr = stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
        if (SUCCEEDED(hr) && read != png.size()) hr = E_FAIL;
        if (FAILED(hr)) png.clear();
        return hr;
    }

    HRESULT EncodeToFile(const PixelBuffer& pixels, const std::wstring& path)
    {
        const bool half = pixels.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!half && !IsBgra8(pixels.format)) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        winrt::com_ptr<IWICImagingFactory> wic;
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        if (FAILED(hr)) return hr;
        winrt::com_ptr<IWICStream> stream;
        hr = wic->CreateStream(stream.put());
        if (FAILED(hr)) return hr;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return hr;
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(half ? GUID_ContainerFormatWmp : GUID_ContainerFormatPng, nullptr, encoder.put());
        if (FAILED(hr)) return hr;
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return hr;
        return WriteFrame(wic.get(), encoder.get(), pixels);
    }
}
