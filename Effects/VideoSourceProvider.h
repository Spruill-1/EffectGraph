#pragma once

#include "pch_engine.h"
#include "VideoFrameConverter.h"
#include "VideoFrameReader.h"
#include "VideoSeekModel.h"

#include <deque>

namespace ShaderLab::Effects
{
    // A video file as a D2D image for the effect graph. The pieces:
    //   - VideoFrameReader: Media Foundation, the stream's format and colour.
    //   - VideoSeekModel: whether moving forward is cheaper as a seek.
    //   - VideoFrameConverter: decoded frame -> linear scRGB FP16 texture.
    //   - this class: the decode thread, its frame queue, seek and playback
    //     state, and the D2D bitmap over the converter's output.
    //
    // A background thread decodes a few frames ahead of the target time into
    // a queue. UploadIfReady() shows the newest queued frame that starts at
    // or before the target and drops older ones, so a late tick skips frames
    // instead of slowing playback.
    class VideoSourceProvider
    {
    public:
        VideoSourceProvider();
        ~VideoSourceProvider();

        // Open a video file. Returns true on success.
        // Requires D3D11 device + context for GPU conversion shader setup.
        bool Open(const std::wstring& filePath, ID2D1DeviceContext5* dc,
                  ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

        // Close and release all resources.
        void Close();

        bool IsOpen() const { return m_reader.IsOpen(); }

        // Free-running playback for Tick (tests drive video this way; the
        // graph drives it through PlayTo).
        void Play();
        bool IsPlaying() const { return m_playing; }
        void SetLoop(bool loop) { m_loop = loop; }
        bool IsLooping() const { return m_loop; }

        // Seek to a position in seconds: flush the queue and decode from the
        // preceding keyframe to the frame that covers it.
        void Seek(double seconds);

        // Show the frame whose [start, start + duration) contains `seconds`.
        // Moving forward decodes on from the current position, discarding
        // frames already in the past; it seeks only when the keyframe the
        // seek would land on is further ahead than SeekThresholdFrames().
        // Moving backward seeks. Calling it again with the same time holds
        // the frame.
        void PlayTo(double seconds);

        // Free-running playback: advance by wall-clock delta, wrapping at the
        // end when looping, and show the frame for that time. Hosts drive
        // video through PlayTo (the node's Time); the playback tests use this.
        void Tick(double deltaSeconds);

        // Step to the frame after the one last uploaded. At the end of the
        // stream a looping video steps back to the first frame.
        void RequestNextFrame();

        // Upload the frame for the current target to the GPU and run the
        // conversion shader. Returns true if a new frame was uploaded.
        // Call on the render thread.
        bool UploadIfReady(ID2D1DeviceContext5* dc);

        // Get the current frame bitmap (may be nullptr if no frame decoded yet).
        ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

        // Video metadata.
        double Duration() const { return Info().durationSeconds; }
        // Where playback is: the seek target until its frame is uploaded, then
        // the start of the uploaded frame.
        double CurrentPosition() const { return m_currentPositionSeconds; }
        uint32_t FrameWidth() const { return Info().width; }
        uint32_t FrameHeight() const { return Info().height; }
        double FrameRate() const { return Info().frameRate; }
        bool IsHDR() const { return Info().color.IsHdr(); }
        const VideoColorDescription& ColorDescription() const { return Info().color; }
        const std::wstring& LastError() const { return m_lastError; }
        const std::wstring& FilePath() const { return m_filePath; }
        uint64_t UploadAttempts() const { return m_uploadAttempts; }
        uint64_t UploadSuccesses() const { return m_uploadSuccesses; }
        uint64_t DecodeCount() const { return m_decodeCount; }
        // Frames decoded into the queue and dropped unshown because a newer
        // frame was already due, and seeks performed.
        uint64_t DroppedFrames() const { return m_droppedFrames; }
        uint64_t SeekCount() const { return m_seekCount; }
        // Measured costs; see VideoSeekModel.
        double DecodeSecondsPerFrame() const { return m_seekModel.DecodeSecondsPerFrame(); }
        double SeekOverheadSeconds() const { return m_seekModel.SeekOverheadSeconds(); }
        double MeanSeekPrerollSeconds() const { return m_seekModel.MeanPrerollSeconds(); }
        double SeekThresholdFrames() const { return m_seekModel.SeekThresholdFrames(); }
        // Keyframes learned from where seeks landed.
        size_t KnownKeyframeCount();
        // Frames the decode thread keeps queued ahead of the shown one.
        static constexpr size_t DecodeAheadFrames() { return cVideoDecodeAheadFrames; }

        // Presentation time (seconds) of the frame most recently uploaded, or
        // -1 before the first upload. Unlike CurrentPosition(), which Seek()
        // sets immediately, this says which frame the output actually holds.
        double UploadedFrameTime() const { return m_uploadedFrameTime.load(); }
        // How long that frame is shown (the sample duration, or 1/fps if the
        // file gives none); 0 before the first upload.
        double UploadedFrameDuration() const { return m_uploadedFrameDuration.load(); }
        // Presentation time of the first frame, which need not be 0 (B-frame
        // reordering delay). Earlier times show this frame.
        double FirstFrameTime() const { return m_firstFrameTime; }

        // The last target handed to Seek(), or NaN once playback has moved
        // the position since. PlayTo holds on it: the frame shown for a
        // target starts at or before it, so comparing the target with
        // CurrentPosition() alone would seek again for any time between two
        // frame starts.
        double LastSeekTarget() const { return m_lastSeekTarget.load(); }

        // Zero-copy upload of hardware-decoded frames (on by default). Off
        // forces the CPU Lock2D path, which exists for software decode; tests
        // use it to compare the two paths on the same frame.
        void SetZeroCopyEnabled(bool enabled) { m_zeroCopyAllowed = enabled; }
        bool LastUploadWasZeroCopy() const { return m_lastUploadZeroCopy; }
        // The FP16 scRGB output texture (for tests / readback).
        ID3D11Texture2D* OutputTexture() const { return m_converter.Output(); }

        // Output format from MF Source Reader.
        using OutputFormat = VideoFrameFormat;
        OutputFormat GetOutputFormat() const { return Info().format; }

    private:
        using DecodedFrame = DecodedVideoFrame;
        enum class DecodeResult { Frame, EndOfStream, Failed, Superseded };
        struct ReadStats
        {
            double firstSampleTime{ -1.0 };      // the keyframe, after a seek
            double secondsToFirstSample{ 0.0 };
        };

        void DecodeThreadFunc(std::stop_token token);
        // Reads samples until one ends after the target time (earlier ones are
        // already in the past and are discarded uncopied) and starts after
        // `skipThrough`, then fills `frame`. A catch-up that takes longer than
        // scCatchUpFrameSeconds stops early at a sample starting at or after
        // `progressFrom`, so the picture keeps moving. Superseded if a seek or
        // Close arrives while it reads.
        DecodeResult DecodeOneFrame(DecodedFrame& frame, uint64_t generation, std::stop_token token,
                                    ReadStats& stats, double skipThrough = -1.0,
                                    double progressFrom = std::numeric_limits<double>::infinity());
        // Moves the target and lets the decode thread fill toward it.
        void SetTarget(double seconds);
        // Removes queued frames a newer queued frame already replaces: m_stateMutex held.
        void PruneQueueLocked(std::vector<DecodedFrame>& released);
        // Keeps a released frame's byte buffer for reuse: m_stateMutex held.
        void RecycleLocked(DecodedFrame& frame);

        const VideoStreamInfo& Info() const { return m_reader.Info(); }

        VideoFrameReader m_reader;
        VideoFrameConverter m_converter;
        VideoSeekModel m_seekModel;

        // D2D output bitmap (shared with the converter's output texture).
        winrt::com_ptr<ID2D1Bitmap1> m_bitmap;
        ID3D11DeviceContext* m_d3dContext{ nullptr };    // Non-owning, from render engine.

        std::atomic<double> m_currentPositionSeconds{ 0.0 };
        std::atomic<uint64_t> m_uploadAttempts{ 0 };
        std::atomic<uint64_t> m_uploadSuccesses{ 0 };
        std::atomic<uint64_t> m_decodeCount{ 0 };
        std::atomic<uint64_t> m_droppedFrames{ 0 };
        std::atomic<uint64_t> m_seekCount{ 0 };

        // Playback state (render thread).
        std::atomic<bool> m_playing{ false };
        bool m_loop{ true };
        double m_playTime{ 0.0 };        // Tick's position
        std::wstring m_lastError;
        std::wstring m_filePath;

        // Background decode thread. m_stateMutex guards the members from
        // m_queue to m_decodeHead, and the seek model's keyframe map; the
        // atomics among them are also read without it.
        std::jthread m_decodeThread;
        std::mutex m_stateMutex;
        std::condition_variable m_decodeCV;
        std::deque<DecodedFrame> m_queue;
        std::vector<std::vector<BYTE>> m_spareBuffers;
        bool m_seekPending{ false };
        double m_seekTarget{ 0.0 };
        bool m_endOfStream{ false };
        bool m_stepRequested{ false };
        // False from a seek until its frame is decoded.
        bool m_seekFrameDecoded{ true };
        // A failed read is retried once by re-seeking to the decode head and
        // skipping the frames already read.
        bool m_resumePending{ false };
        bool m_readRetried{ false };
        // Bumped by every seek; a frame decoded for an older one is discarded.
        std::atomic<uint64_t> m_generation{ 0 };
        std::atomic<double> m_targetTime{ 0.0 };
        // Start of the newest frame read in this generation, and never behind
        // the seek target: where forward decoding continues from.
        double m_decodeHead{ 0.0 };

        std::atomic<double> m_uploadedFrameTime{ -1.0 };
        std::atomic<double> m_uploadedFrameDuration{ 0.0 };
        uint64_t m_uploadedGeneration{ 0 };   // render thread
        double m_firstFrameTime{ 0.0 };

        // Zero-copy path: the decode thread queues hardware-decoded samples
        // (holding one keeps its decoder surface alive) and the converter
        // copies them GPU-side.
        bool m_zeroCopyAllowed{ true };
        std::atomic<bool> m_zeroCopyFailed{ false };
        bool m_lastUploadZeroCopy{ false };

        std::atomic<double> m_lastSeekTarget{ std::numeric_limits<double>::quiet_NaN() };
    };
}
