#pragma once

#include "pch_engine.h"

namespace ShaderLab::Effects
{
    // The pixel layout the Source Reader delivers.
    enum class VideoFrameFormat { RGB32, NV12, P010 };

    // How a stream's code values become linear light: one description, read
    // once from the native media type, that picks the decode format and
    // drives the conversion shader. An attribute the file does not carry is
    // Unknown, and DescribeVideoColor resolves it from the others.
    struct VideoColorDescription
    {
        enum class Transfer : uint8_t { Unknown, Sdr, Pq, Hlg };      // Sdr: the sRGB curve
        enum class Primaries : uint8_t { Unknown, Bt709, Bt2020 };
        enum class Matrix : uint8_t { Unknown, Bt601, Bt709, Bt2020 };
        enum class Range : uint8_t { Unknown, Limited, Full };

        Transfer transfer{ Transfer::Unknown };
        Primaries primaries{ Primaries::Unknown };
        Matrix matrix{ Matrix::Unknown };
        Range range{ Range::Unknown };

        // PQ or HLG: decoded through 10-bit P010.
        bool IsHdr() const { return transfer == Transfer::Pq || transfer == Transfer::Hlg; }
        bool operator==(const VideoColorDescription&) const = default;
    };

    // The raw MF attributes a description is made from (0 = absent), so the
    // rules can be tested without a file.
    struct VideoColorTags
    {
        UINT32 transferFunction{ 0 };   // MF_MT_TRANSFER_FUNCTION
        UINT32 primaries{ 0 };          // MF_MT_VIDEO_PRIMARIES
        UINT32 yuvMatrix{ 0 };          // MF_MT_YUV_MATRIX
        UINT32 nominalRange{ 0 };       // MF_MT_VIDEO_NOMINAL_RANGE
        bool hevcMain10{ false };       // HEVC with MF_MT_MPEG2_PROFILE 2
    };

    // The stream's colour, every field resolved:
    //   - transfer: PQ and HLG as tagged. Untagged, the stream is PQ when it
    //     has BT.2020 primaries or is HEVC Main 10, and SDR otherwise.
    //   - primaries: only BT.709 and BT.2020 are modelled; any other tag
    //     (DCI-P3 among them) is treated as BT.709. Untagged, BT.2020 for
    //     HDR and BT.709 for SDR.
    //   - matrix: as tagged; untagged, BT.2020 for HDR and BT.709 for SDR.
    //   - range: as tagged; untagged, limited.
    // The file name plays no part.
    VideoColorDescription DescribeVideoColor(const VideoColorTags& tags);
    VideoColorTags ReadVideoColorTags(IMFMediaType* nativeType);

    // A decoded frame waiting in the provider's queue: raw bytes (CPU path)
    // or the decoder's sample (zero-copy path). The provider stamps the
    // generation and seek flag it was decoded for.
    struct DecodedVideoFrame
    {
        struct GpuSurface
        {
            winrt::com_ptr<IMFSample>       sample;
            winrt::com_ptr<ID3D11Texture2D> tex;
            UINT                            subresource{ 0 };
        };

        double time{ -1.0 };
        double duration{ 0.0 };
        uint64_t generation{ 0 };
        bool seekFrame{ false };    // first frame after a seek: shown whatever the target
        std::vector<BYTE> bytes;
        LONG pitch{ 0 };
        GpuSurface gpu;
    };

    // What Open learned about the stream.
    struct VideoStreamInfo
    {
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        uint32_t stride{ 0 };
        double frameRate{ 0.0 };
        double frameDuration{ 0.0 };
        double durationSeconds{ 0.0 };
        VideoFrameFormat format{ VideoFrameFormat::RGB32 };
        VideoColorDescription color;
    };

    // The Media Foundation side of a video source: creates the Source Reader
    // (hardware decode first, then the video processor, then a bare reader),
    // describes the stream's colour, negotiates the output format, and copies
    // a decoded sample out. No D2D. The reader itself is used from the
    // provider's decode thread; Open and Close run with that thread stopped.
    class VideoFrameReader
    {
    public:
        VideoFrameReader() = default;
        VideoFrameReader(const VideoFrameReader&) = delete;
        VideoFrameReader& operator=(const VideoFrameReader&) = delete;
        ~VideoFrameReader();

        // False with LastError() set.
        bool Open(const std::wstring& filePath, ID3D11Device* d3dDevice);
        void Close();

        bool IsOpen() const { return m_reader != nullptr; }
        IMFSourceReader* Reader() const { return m_reader.get(); }
        const VideoStreamInfo& Info() const { return m_info; }
        const std::wstring& LastError() const { return m_lastError; }

        // Fills frame.bytes / frame.pitch from the sample, or frame.gpu with
        // the decoder's texture when zero-copy is allowed and the sample is
        // one. False if the sample's buffer cannot be read.
        bool CopySample(IMFSample* sample, DecodedVideoFrame& frame, bool allowZeroCopy);

    private:
        bool NegotiateOutputFormat();

        winrt::com_ptr<IMFSourceReader> m_reader;
        winrt::com_ptr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
        UINT m_resetToken{ 0 };
        bool m_mfStarted{ false };
        bool m_firstFrameLogged{ false };
        VideoStreamInfo m_info;
        std::wstring m_lastError;
    };
}
