#pragma once

#include "pch_engine.h"
#include "VideoFrameReader.h"

namespace ShaderLab::Effects
{
    // Turns decoded video frames into a linear scRGB FP16 texture on the GPU:
    // the upload textures for CPU-copied frames, the planar texture for
    // zero-copy decoder surfaces, and one compute shader compiled from a
    // single template for the stream's format and colour description.
    //
    // Threading: render thread. Each conversion is recorded on a deferred
    // context and submitted with one ExecuteCommandList, because the
    // immediate context is shared with D2D and multithread protection
    // serializes single calls, not sequences of them.
    class VideoFrameConverter
    {
    public:
        VideoFrameConverter() = default;
        VideoFrameConverter(const VideoFrameConverter&) = delete;
        VideoFrameConverter& operator=(const VideoFrameConverter&) = delete;

        // False with LastError() set.
        bool Create(ID3D11Device* device, ID3D11DeviceContext* immediateContext,
                    VideoFrameFormat format, const VideoColorDescription& color,
                    uint32_t width, uint32_t height);
        void Reset();

        // Converts a CPU-copied frame (DecodedVideoFrame::bytes / pitch).
        bool ConvertBytes(const BYTE* data, UINT pitch);
        // Whether decoder surfaces like `decoderTexture` can be converted
        // directly; creates the planar texture on first use.
        bool CanConvertSurface(ID3D11Texture2D* decoderTexture);
        // Converts a decoder surface: one GPU copy into the planar texture,
        // recorded with the conversion.
        bool ConvertSurface(ID3D11Texture2D* decoderTexture, UINT subresource);

        // The FP16 scRGB output, which the provider wraps as its D2D bitmap.
        ID3D11Texture2D* Output() const { return m_texOutput.get(); }
        const std::wstring& LastError() const { return m_lastError; }

        // The HLSL Create compiles for a format and colour description.
        static std::string ShaderSource(VideoFrameFormat format, const VideoColorDescription& color);

    private:
        bool CreateResources();
        bool CompileShader();
        // Records the dispatch (after an optional decoder-surface copy) and submits it.
        bool Run(bool planar, ID3D11Texture2D* decoderTex, UINT decoderSubresource);

        ID3D11Device* m_device{ nullptr };               // non-owning, from the render engine
        ID3D11DeviceContext* m_immediate{ nullptr };     // non-owning, from the render engine
        // Null if a deferred context is unavailable; then the immediate
        // context is used directly.
        winrt::com_ptr<ID3D11DeviceContext> m_convertCtx;
        VideoFrameFormat m_format{ VideoFrameFormat::RGB32 };
        VideoColorDescription m_color;
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };

        winrt::com_ptr<ID3D11ComputeShader> m_shader;
        winrt::com_ptr<ID3D11Texture2D> m_texY;        // Y plane input (CPU path)
        winrt::com_ptr<ID3D11Texture2D> m_texUV;       // UV plane input (CPU path)
        winrt::com_ptr<ID3D11Texture2D> m_texRGB;      // RGB32 input
        winrt::com_ptr<ID3D11Texture2D> m_texOutput;   // scRGB FP16 output
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvY;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvUV;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvRGB;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_uavOutput;
        winrt::com_ptr<ID3D11Buffer> m_cbParams;       // dimensions

        // Zero-copy: the decoder surface is copied GPU-side into this planar
        // texture, whose planes the shader reads through R8/R8G8 (NV12) or
        // R16/R16G16 (P010) views.
        winrt::com_ptr<ID3D11Texture2D>          m_texPlanar;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvPlanarY;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvPlanarUV;

        std::wstring m_lastError;
    };
}
