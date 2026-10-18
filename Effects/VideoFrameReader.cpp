#include "pch_engine.h"
#include "VideoFrameReader.h"

namespace ShaderLab::Effects
{
    namespace
    {
        // Media Foundation stays started while any reader is open.
        std::atomic<int> sMfRefCount{ 0 };

        void StartMediaFoundation()
        {
            if (sMfRefCount.fetch_add(1) == 0)
                MFStartup(MF_VERSION);
        }

        void StopMediaFoundation()
        {
            if (sMfRefCount.fetch_sub(1) == 1)
                MFShutdown();
        }

        const wchar_t* FormatName(VideoFrameFormat format)
        {
            switch (format)
            {
            case VideoFrameFormat::P010: return L"P010";
            case VideoFrameFormat::NV12: return L"NV12";
            default:                     return L"RGB32";
            }
        }
    }

    VideoColorTags ReadVideoColorTags(IMFMediaType* nativeType)
    {
        VideoColorTags tags;
        if (!nativeType) return tags;
        nativeType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &tags.transferFunction);
        nativeType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &tags.primaries);
        nativeType->GetUINT32(MF_MT_YUV_MATRIX, &tags.yuvMatrix);
        nativeType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &tags.nominalRange);
        GUID subtype{};
        nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 profile = 0;
        nativeType->GetUINT32(MF_MT_MPEG2_PROFILE, &profile);
        tags.hevcMain10 = (subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265) && profile == 2;
        return tags;
    }

    VideoColorDescription DescribeVideoColor(const VideoColorTags& tags)
    {
        using D = VideoColorDescription;
        D color;

        const bool taggedBt2020 = tags.primaries == MFVideoPrimaries_BT2020;
        switch (tags.transferFunction)
        {
        case MFVideoTransFunc_Unknown:
            color.transfer = (taggedBt2020 || tags.hevcMain10) ? D::Transfer::Pq : D::Transfer::Sdr;
            break;
        case MFVideoTransFunc_2084: color.transfer = D::Transfer::Pq;  break;
        case MFVideoTransFunc_HLG:  color.transfer = D::Transfer::Hlg; break;
        default:                    color.transfer = D::Transfer::Sdr; break;
        }
        const bool hdr = color.IsHdr();

        if (tags.primaries == MFVideoPrimaries_Unknown)
            color.primaries = hdr ? D::Primaries::Bt2020 : D::Primaries::Bt709;
        else
            color.primaries = taggedBt2020 ? D::Primaries::Bt2020 : D::Primaries::Bt709;

        switch (tags.yuvMatrix)
        {
        case MFVideoTransferMatrix_BT601:     color.matrix = D::Matrix::Bt601;  break;
        case MFVideoTransferMatrix_BT709:
        case MFVideoTransferMatrix_SMPTE240M: color.matrix = D::Matrix::Bt709;  break;
        case MFVideoTransferMatrix_BT2020_10:
        case MFVideoTransferMatrix_BT2020_12: color.matrix = D::Matrix::Bt2020; break;
        default:                              color.matrix = hdr ? D::Matrix::Bt2020 : D::Matrix::Bt709; break;
        }

        color.range = tags.nominalRange == MFNominalRange_0_255 ? D::Range::Full : D::Range::Limited;
        return color;
    }

    VideoFrameReader::~VideoFrameReader()
    {
        Close();
    }

    bool VideoFrameReader::Open(const std::wstring& filePath, ID3D11Device* d3dDevice)
    {
        Close();
        m_lastError.clear();
        if (filePath.empty())
        {
            m_lastError = L"Empty file path";
            return false;
        }

        StartMediaFoundation();
        m_mfStarted = true;

        // Device manager for hardware-accelerated decode.
        UINT resetToken = 0;
        winrt::com_ptr<IMFDXGIDeviceManager> dxgiManager;
        HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, dxgiManager.put());
        if (SUCCEEDED(hr))
        {
            hr = dxgiManager->ResetDevice(d3dDevice, resetToken);
            if (SUCCEEDED(hr))
            {
                m_dxgiDeviceManager = dxgiManager;
                m_resetToken = resetToken;
            }
        }

        // Hardware decode through the device manager first, then the video
        // processor (software format conversion), then a bare reader.
        auto tryCreateReader = [&](bool useDxgi, bool useVideoProc) -> HRESULT
        {
            winrt::com_ptr<IMFAttributes> attrs;
            HRESULT hres = MFCreateAttributes(attrs.put(), 3);
            if (FAILED(hres)) return hres;

            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            if (useDxgi && m_dxgiDeviceManager)
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiDeviceManager.get());
            if (useVideoProc)
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            return MFCreateSourceReaderFromURL(filePath.c_str(), attrs.get(), m_reader.put());
        };

        int readerAttempt = 1;
        hr = tryCreateReader(true, false);
        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[VideoSource] Attempt 1 (DXGI) failed: 0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            m_dxgiDeviceManager = nullptr;
            readerAttempt = 2;
            hr = tryCreateReader(false, true);
        }
        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[VideoSource] Attempt 2 (video proc) failed: 0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            readerAttempt = 3;
            hr = tryCreateReader(false, false);
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateSourceReaderFromURL failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }
        OutputDebugStringW(std::format(L"[VideoSource] Reader created via attempt {}\n", readerAttempt).c_str());

        m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        winrt::com_ptr<IMFMediaType> nativeType;
        hr = m_reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nativeType.put());
        if (FAILED(hr))
        {
            m_lastError = std::format(L"GetNativeMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        const VideoColorTags tags = ReadVideoColorTags(nativeType.get());
        m_info.color = DescribeVideoColor(tags);
        OutputDebugStringW(std::format(
            L"[VideoSource] Colour tags: transfer={}, primaries={}, matrix={}, range={}, hevcMain10={}; HDR={}\n",
            tags.transferFunction, tags.primaries, tags.yuvMatrix, tags.nominalRange, tags.hevcMain10,
            m_info.color.IsHdr()).c_str());

        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(nativeType.get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr)) { m_lastError = L"Failed to get frame size"; Close(); return false; }
        m_info.width = width;
        m_info.height = height;

        UINT32 fpsNum = 0, fpsDen = 0;
        hr = MFGetAttributeRatio(nativeType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        m_info.frameRate = (SUCCEEDED(hr) && fpsDen > 0) ? static_cast<double>(fpsNum) / fpsDen : 30.0;
        m_info.frameDuration = 1.0 / m_info.frameRate;

        PROPVARIANT var;
        PropVariantInit(&var);
        hr = m_reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var);
        if (SUCCEEDED(hr))
        {
            m_info.durationSeconds = static_cast<double>(var.uhVal.QuadPart) / 10'000'000.0;
            PropVariantClear(&var);
        }

        if (!NegotiateOutputFormat())
        {
            Close();
            return false;
        }
        return true;
    }

    bool VideoFrameReader::NegotiateOutputFormat()
    {
        // HDR decodes to P010, SDR to NV12; RGB32 is the fallback for both
        // (the video processor does the YUV->RGB conversion).
        const bool hdr = m_info.color.IsHdr();
        auto trySubtype = [&](const GUID& subtype, uint32_t stride) -> HRESULT
        {
            winrt::com_ptr<IMFMediaType> outputType;
            MFCreateMediaType(outputType.put());
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, subtype);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_info.width, m_info.height);
            m_info.stride = stride;
            return m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        };

        HRESULT hr = hdr ? trySubtype(MFVideoFormat_P010, m_info.width * 2) : trySubtype(MFVideoFormat_NV12, m_info.width);
        if (SUCCEEDED(hr))
        {
            m_info.format = hdr ? VideoFrameFormat::P010 : VideoFrameFormat::NV12;
            if (hdr) OutputDebugStringW(L"[VideoSource] P010 format accepted\n");
        }
        else
        {
            if (hdr)
                OutputDebugStringW(std::format(L"[VideoSource] P010 rejected: 0x{:08X}, falling back to RGB32\n",
                    static_cast<uint32_t>(hr)).c_str());
            hr = trySubtype(MFVideoFormat_RGB32, m_info.width * 4);
            if (SUCCEEDED(hr)) m_info.format = VideoFrameFormat::RGB32;
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"SetCurrentMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // The confirmed output type's stride, when it states one.
        winrt::com_ptr<IMFMediaType> actualType;
        hr = m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actualType.put());
        if (SUCCEEDED(hr))
        {
            INT32 actualStride = 0;
            if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&actualStride))) &&
                actualStride != 0)
                m_info.stride = static_cast<uint32_t>(actualStride < 0 ? -actualStride : actualStride);
        }
        return true;
    }

    void VideoFrameReader::Close()
    {
        m_reader = nullptr;
        m_dxgiDeviceManager = nullptr;
        m_resetToken = 0;
        m_firstFrameLogged = false;
        m_info = {};
        if (m_mfStarted)
        {
            StopMediaFoundation();
            m_mfStarted = false;
        }
    }

    bool VideoFrameReader::CopySample(IMFSample* sample, DecodedVideoFrame& frame, bool allowZeroCopy)
    {
        const uint32_t width = m_info.width;
        const uint32_t height = m_info.height;
        const VideoFrameFormat format = m_info.format;

        winrt::com_ptr<IMFMediaBuffer> buffer;
        DWORD bufCount = 0;
        sample->GetBufferCount(&bufCount);
        HRESULT hr = bufCount > 0
            ? sample->GetBufferByIndex(0, buffer.put())
            : sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr) || !buffer) return false;

        // Hardware decode: queue the decoder texture instead of reading it
        // back; the converter copies it GPU-side.
        if (allowZeroCopy && format != VideoFrameFormat::RGB32)
        {
            winrt::com_ptr<IMFDXGIBuffer> dxgiBuffer;
            if (buffer.try_as(dxgiBuffer))
            {
                DecodedVideoFrame::GpuSurface gpu;
                if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(gpu.tex.put()))) &&
                    SUCCEEDED(dxgiBuffer->GetSubresourceIndex(&gpu.subresource)) && gpu.tex)
                {
                    gpu.sample.copy_from(sample);
                    frame.gpu = std::move(gpu);
                    return true;
                }
            }
        }
        frame.gpu = {};

        // CPU path: Lock2D reads a GPU buffer back when the decoder is hardware.
        winrt::com_ptr<IMF2DBuffer> buffer2D;
        buffer.try_as(buffer2D);

        BYTE* data = nullptr;
        LONG pitch = 0;
        bool locked2D = false;

        if (buffer2D)
        {
            hr = buffer2D->Lock2D(&data, &pitch);
            locked2D = SUCCEEDED(hr);
        }
        DWORD lockedLength = 0;   // bytes behind `data`, when known
        if (locked2D)
        {
            buffer2D->GetContiguousLength(&lockedLength);
        }
        else
        {
            DWORD maxLen = 0;
            hr = buffer->Lock(&data, &maxLen, &lockedLength);
            if (FAILED(hr)) return false;
            pitch = static_cast<LONG>(m_info.stride);
        }

        if (!m_firstFrameLogged)
        {
            m_firstFrameLogged = true;
            winrt::com_ptr<IMFDXGIBuffer> dxgiBuf;
            buffer.try_as(dxgiBuf);
            OutputDebugStringW(std::format(
                L"[VideoSource] First frame: format={}, DXVA={}, locked2D={}, pitch={}, stride={}, {}x{}\n",
                FormatName(format), dxgiBuf != nullptr, locked2D, pitch, m_info.stride, width, height).c_str());
        }

        // Raw byte copy; the conversion shader does all colour work.
        LONG absPitch = (pitch < 0) ? -pitch : pitch;
        const size_t yPlaneSize = static_cast<size_t>(absPitch) * height;
        const size_t totalSize = format == VideoFrameFormat::RGB32
            ? yPlaneSize : yPlaneSize + static_cast<size_t>(absPitch) * (height / 2);

        if (frame.bytes.size() < totalSize)
            frame.bytes.resize(totalSize);

        // The chroma plane follows the ALLOCATED luma rows, not the frame's:
        // decoders round the surface height up to their block size (1080 is
        // stored as 1088). The allocated height is recoverable from the
        // buffer size: luma rows plus half as many chroma rows, one pitch each.
        uint32_t surfaceRows = height;
        if (format != VideoFrameFormat::RGB32 && absPitch > 0 && lockedLength > 0)
        {
            const uint64_t rows = (static_cast<uint64_t>(lockedLength) * 2) /
                                  (static_cast<uint64_t>(absPitch) * 3);
            if (rows >= height) surfaceRows = static_cast<uint32_t>(rows);
        }

        if (pitch > 0 && surfaceRows == height)
        {
            std::memcpy(frame.bytes.data(), data, totalSize);
        }
        else
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const BYTE* srcRow = data + static_cast<ptrdiff_t>(y) * pitch;
                BYTE* dstRow = frame.bytes.data() + static_cast<size_t>(y) * absPitch;
                std::memcpy(dstRow, srcRow, absPitch);
            }
            if (format != VideoFrameFormat::RGB32)
            {
                const BYTE* uvSrc = data + static_cast<ptrdiff_t>(surfaceRows) * pitch;
                BYTE* uvDst = frame.bytes.data() + yPlaneSize;
                for (uint32_t y = 0; y < height / 2; ++y)
                {
                    std::memcpy(uvDst + static_cast<size_t>(y) * absPitch,
                                uvSrc + static_cast<ptrdiff_t>(y) * pitch, absPitch);
                }
            }
        }
        frame.pitch = absPitch;

        if (locked2D) buffer2D->Unlock2D();
        else buffer->Unlock();

        return true;
    }
}
