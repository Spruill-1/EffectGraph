#include "pch_engine.h"
#include "VideoSourceProvider.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    VideoSourceProvider::VideoSourceProvider() = default;

    VideoSourceProvider::~VideoSourceProvider()
    {
        Close();
    }

    bool VideoSourceProvider::Open(const std::wstring& filePath, ID2D1DeviceContext5* dc,
                                    ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    {
        Close();
        m_lastError.clear();
        m_filePath = filePath;
        m_d3dContext = d3dContext;

        if (!m_reader.Open(filePath, d3dDevice))
        {
            m_lastError = m_reader.LastError();
            Close();
            return false;
        }
        const VideoStreamInfo& info = Info();
        m_seekModel.Reset(info.frameRate);

        // The converter's output, viewed by D2D as the node's image.
        winrt::com_ptr<IDXGISurface> surface;
        if (!dc || !m_converter.Create(d3dDevice, d3dContext, info.format, info.color, info.width, info.height))
        {
            m_lastError = dc ? m_converter.LastError() : L"Failed to create GPU conversion resources: no device context";
            Close();
            return false;
        }
        D2D1_BITMAP_PROPERTIES1 bmpProps{};
        bmpProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        bmpProps.dpiX = 96.0f; bmpProps.dpiY = 96.0f;
        bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        if (FAILED(m_converter.Output()->QueryInterface(IID_PPV_ARGS(surface.put()))) ||
            FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_bitmap.put())))
        {
            m_lastError = L"Failed to create GPU conversion resources: CreateBitmapFromDxgiSurface";
            Close();
            return false;
        }

        // Decode the first frame synchronously so the node has an image at once.
        {
            DecodedFrame first;
            ReadStats stats;
            if (DecodeOneFrame(first, 0, {}, stats) == DecodeResult::Frame)
            {
                first.seekFrame = true;
                m_firstFrameTime = first.time;
                std::lock_guard lock(m_stateMutex);
                m_decodeHead = first.time;
                m_seekModel.RecordKeyframe(first.time, first.time);
                m_queue.push_back(std::move(first));
            }
        }

        m_decodeThread = std::jthread([this](std::stop_token token) { DecodeThreadFunc(token); });

        const wchar_t* fmtName = (info.format == VideoFrameFormat::P010) ? L"P010"
            : (info.format == VideoFrameFormat::NV12) ? L"NV12" : L"RGB32";
        OutputDebugStringW(std::format(L"[VideoSource] Opened: {}x{} @ {:.1f}fps, {:.1f}s, HDR={}, format={}\n",
            info.width, info.height, info.frameRate, info.durationSeconds, info.color.IsHdr(), fmtName).c_str());

        return true;
    }

    void VideoSourceProvider::Close()
    {
        if (m_decodeThread.joinable())
        {
            // Release the queued frames first: their decoder surfaces are what
            // a ReadSample blocked on a full surface pool is waiting for.
            std::vector<DecodedFrame> released;
            {
                std::lock_guard lock(m_stateMutex);
                m_decodeThread.request_stop();
                for (auto& frame : m_queue) released.push_back(std::move(frame));
                m_queue.clear();
            }
            released.clear();
            m_decodeCV.notify_all();
            m_decodeThread.join();
        }

        // Held samples pin decoder surfaces: release them before the reader.
        m_queue.clear();
        m_spareBuffers.clear();
        m_seekPending = false;
        m_seekTarget = 0.0;
        m_endOfStream = false;
        m_stepRequested = false;
        m_seekFrameDecoded = true;
        m_resumePending = false;
        m_readRetried = false;
        m_seekModel.Reset(0.0);
        m_generation = 0;
        m_targetTime = 0.0;
        m_decodeHead = 0.0;
        m_uploadedGeneration = 0;
        m_playTime = 0.0;
        m_zeroCopyFailed = false;
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        m_bitmap = nullptr;
        m_converter.Reset();
        m_d3dContext = nullptr;
        m_currentPositionSeconds = 0.0;
        m_playing = false;
        m_uploadedFrameTime = -1.0;
        m_uploadedFrameDuration = 0.0;
        m_firstFrameTime = 0.0;
        m_reader.Close();
    }

    // -----------------------------------------------------------------------
    // Playback controls (render thread)
    // -----------------------------------------------------------------------

    // A frame starting within this of a target counts as starting at it.
    static constexpr double scTimeTolerance = cVideoTimeTolerance;
    // A read loop this long times decode throughput. Single reads after an
    // idle wait come from the decoder's pipeline and are far cheaper.
    static constexpr int scMinTimedReads = 8;
    // Longest a catch-up decodes without delivering a frame.
    static constexpr double scCatchUpFrameSeconds = 0.1;

    static double SecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    void VideoSourceProvider::Play()
    {
        if (!IsOpen()) return;
        // A video that played to its end without looping starts over.
        const double duration = Info().durationSeconds;
        if (!m_loop && duration > 0.0 && m_playTime >= duration - Info().frameDuration)
            Seek(0.0);
        m_playing = true;
    }

    void VideoSourceProvider::Seek(double seconds)
    {
        if (!IsOpen()) return;
        const double duration = Info().durationSeconds;
        seconds = duration > 0.0 ? (std::clamp)(seconds, 0.0, duration) : (std::max)(seconds, 0.0);
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            ++m_generation;
            m_seekTarget = seconds;
            m_seekPending = true;
            m_seekFrameDecoded = false;
            m_resumePending = false;
            m_readRetried = false;
            m_stepRequested = false;
            m_targetTime = seconds;
            m_decodeHead = seconds;
            for (auto& frame : m_queue)
            {
                RecycleLocked(frame);
                released.push_back(std::move(frame));
            }
            m_queue.clear();
        }
        m_decodeCV.notify_one();
        ++m_seekCount;
        m_playTime = seconds;
        m_currentPositionSeconds = seconds;
        m_lastSeekTarget = seconds;
    }

    size_t VideoSourceProvider::KnownKeyframeCount()
    {
        std::lock_guard lock(m_stateMutex);
        return m_seekModel.KnownKeyframeCount();
    }

    void VideoSourceProvider::PlayTo(double seconds)
    {
        if (!IsOpen()) return;
        seconds = (std::max)(seconds, 0.0);
        // Past the end holds the last frame, which ends at or after the duration.
        const double duration = Info().durationSeconds;
        if (duration > 0.0)
            seconds = (std::min)(seconds, duration - 2.0 * scTimeTolerance);
        m_playTime = seconds;
        // A seek to this time is already delivering its frame.
        if (seconds == m_lastSeekTarget.load()) return;

        bool seek;
        {
            std::lock_guard lock(m_stateMutex);
            // The earliest time reachable without a seek is the start of the
            // frame on screen, or the pending seek's target. The first frame
            // also covers any earlier time.
            const bool shownThisGeneration = m_uploadedGeneration == m_generation.load() && m_uploadedFrameTime.load() >= 0.0;
            const double earliest = shownThisGeneration ? m_uploadedFrameTime.load() : m_seekTarget;
            const bool behind = earliest > m_firstFrameTime + scTimeTolerance && seconds < earliest - scTimeTolerance;
            seek = behind || m_seekModel.ForwardSeekPays(seconds, m_decodeHead, Info().frameDuration, m_seekFrameDecoded);
        }
        if (seek)
        {
            Seek(seconds);
            return;
        }
        SetTarget(seconds);
    }

    void VideoSourceProvider::SetTarget(double seconds)
    {
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            m_targetTime = seconds;
            PruneQueueLocked(released);
        }
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::Tick(double deltaSeconds)
    {
        if (!IsOpen() || !m_playing) return;
        double seconds = m_playTime + deltaSeconds;
        const double duration = Info().durationSeconds;
        if (duration > 0.0 && seconds >= duration)
        {
            if (m_loop)
            {
                seconds = std::fmod(seconds, duration);
            }
            else
            {
                // Hold the last frame, which ends at or after the duration.
                seconds = duration - 2.0 * scTimeTolerance;
                m_playing = false;
            }
        }
        PlayTo(seconds);
    }

    void VideoSourceProvider::RequestNextFrame()
    {
        if (!IsOpen()) return;
        // Until a seek's frame is up there is nothing to step from.
        const double shown = m_uploadedFrameTime.load();
        if (shown < 0.0 || m_uploadedGeneration != m_generation.load()) return;
        const double next = shown + (std::max)(m_uploadedFrameDuration.load(), 2.0 * scTimeTolerance);
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        m_playTime = next;
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            m_targetTime = next;
            m_stepRequested = true;
            PruneQueueLocked(released);
        }
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::PruneQueueLocked(std::vector<DecodedFrame>& released)
    {
        const double target = m_targetTime.load();
        while (m_queue.size() >= 2 && m_queue[1].time <= target + scTimeTolerance)
        {
            RecycleLocked(m_queue.front());
            released.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
            ++m_droppedFrames;
        }
    }

    void VideoSourceProvider::RecycleLocked(DecodedFrame& frame)
    {
        if (!frame.bytes.empty() && m_spareBuffers.size() < cVideoDecodeAheadFrames + 2)
            m_spareBuffers.push_back(std::move(frame.bytes));
        frame.bytes = {};
    }

    // -----------------------------------------------------------------------
    // Upload (render thread) — GPU copy or CPU upload, then run conversion shader
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::UploadIfReady(ID2D1DeviceContext5* dc)
    {
        m_uploadAttempts++;
        if (!IsOpen() || !m_d3dContext || !dc) return false;

        // Take the newest queued frame due at the target; older ones are dropped.
        DecodedFrame frame;
        bool haveFrame = false;
        bool restart = false;
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            const double target = m_targetTime.load();
            size_t due = 0;
            while (due < m_queue.size() &&
                   (m_queue[due].seekFrame || m_queue[due].time <= target + scTimeTolerance))
                ++due;
            if (due > 0)
            {
                for (size_t i = 0; i + 1 < due; ++i)
                {
                    RecycleLocked(m_queue[i]);
                    released.push_back(std::move(m_queue[i]));
                    ++m_droppedFrames;
                }
                frame = std::move(m_queue[due - 1]);
                m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<ptrdiff_t>(due));
                m_stepRequested = false;
                haveFrame = true;
            }
            else
            {
                restart = m_queue.empty() && m_endOfStream && m_loop && m_stepRequested;
            }
        }
        released.clear();
        if (restart)
        {
            Seek(0.0);
            return false;
        }
        if (!haveFrame) return false;
        m_decodeCV.notify_one();   // the queue has room again

        bool converted = false;
        if (frame.gpu.tex)
        {
            // Zero-copy: the decoder surface is already on this device. One
            // GPU copy into the planar texture (D3D11 copies both planes of
            // an NV12/P010 subresource together), then convert from it.
            if (!m_converter.CanConvertSurface(frame.gpu.tex.get()))
            {
                // Unsupported here: use the CPU path from now on, and decode
                // this position again so the frame is not simply lost.
                m_zeroCopyFailed = true;
                frame = {};
                Seek(m_targetTime.load());
                return false;
            }
            converted = m_converter.ConvertSurface(frame.gpu.tex.get(), frame.gpu.subresource);
            m_lastUploadZeroCopy = true;
        }
        else
        {
            m_lastUploadZeroCopy = false;
            converted = m_converter.ConvertBytes(frame.bytes.data(), static_cast<UINT>(frame.pitch));
        }
        if (!converted)
            m_lastError = m_converter.LastError();

        {
            std::lock_guard lock(m_stateMutex);
            RecycleLocked(frame);
        }
        // Releasing the sample hands its surface back to the decoder.
        frame.gpu = {};

        if (!converted) return false;
        m_uploadSuccesses++;
        m_uploadedGeneration = frame.generation;
        m_uploadedFrameDuration = frame.duration;
        m_uploadedFrameTime = frame.time;
        m_currentPositionSeconds = frame.time;
        return true;
    }

    // -----------------------------------------------------------------------
    // Background decode thread — raw byte copy only, no color conversion
    // -----------------------------------------------------------------------

    void VideoSourceProvider::DecodeThreadFunc(std::stop_token token)
    {
        for (;;)
        {
            bool seek = false;
            bool resume = false;
            double seekTarget = 0.0;
            double skipThrough = -1.0;
            uint64_t generation = 0;
            DecodedFrame frame;
            {
                std::unique_lock lock(m_stateMutex);
                m_decodeCV.wait(lock, [&] {
                    return token.stop_requested() || m_seekPending || m_resumePending ||
                           (!m_endOfStream && m_queue.size() < cVideoDecodeAheadFrames);
                });
                if (token.stop_requested()) return;
                if (m_seekPending)
                {
                    seek = true;
                    seekTarget = m_seekTarget;
                    m_seekPending = false;
                    m_endOfStream = false;
                }
                else if (m_resumePending)
                {
                    resume = true;
                    skipThrough = m_decodeHead;
                    m_resumePending = false;
                }
                generation = m_generation.load();
                if (!m_spareBuffers.empty())
                {
                    frame.bytes = std::move(m_spareBuffers.back());
                    m_spareBuffers.pop_back();
                }
            }

            bool positioned = true;
            if (seek || resume)
            {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = static_cast<LONGLONG>((seek ? seekTarget : skipThrough) * 10'000'000.0);
                positioned = SUCCEEDED(m_reader.Reader()->SetCurrentPosition(GUID_NULL, position));
                PropVariantClear(&position);
            }
            ReadStats stats;
            const DecodeResult result = positioned
                ? DecodeOneFrame(frame, generation, token, stats, skipThrough,
                                 seek ? seekTarget : -std::numeric_limits<double>::infinity())
                : DecodeResult::Failed;
            if (seek && result == DecodeResult::Frame)
                m_seekModel.RecordSeekOverhead(stats.secondsToFirstSample);

            std::vector<DecodedFrame> released;
            std::lock_guard lock(m_stateMutex);
            if (seek && stats.firstSampleTime >= 0.0)
            {
                m_seekModel.RecordKeyframe(stats.firstSampleTime, seekTarget);
                if (stats.firstSampleTime <= seekTarget + scTimeTolerance)
                    m_seekModel.RecordPreroll((std::max)(seekTarget - stats.firstSampleTime, scTimeTolerance));
            }
            const bool current = generation == m_generation.load();
            if (seek && current && result != DecodeResult::Superseded)
                m_seekFrameDecoded = true;
            if (token.stop_requested() || !current || result != DecodeResult::Frame)
            {
                RecycleLocked(frame);
                released.push_back(std::move(frame));
                if (current && result == DecodeResult::Failed && !m_readRetried)
                {
                    // Retry a failed read once: redo the seek, or re-position
                    // at the decode head and skip what was already read.
                    m_readRetried = true;
                    if (seek)
                    {
                        m_seekPending = true;
                        m_seekFrameDecoded = false;
                    }
                    else
                    {
                        m_resumePending = true;
                    }
                }
                else if (current && (result == DecodeResult::EndOfStream || result == DecodeResult::Failed))
                {
                    // Decoding stops until the next seek.
                    m_endOfStream = true;
                }
                continue;
            }
            m_readRetried = false;
            frame.generation = generation;
            frame.seekFrame = seek;
            m_queue.push_back(std::move(frame));
            ++m_decodeCount;
            PruneQueueLocked(released);
        }
    }

    VideoSourceProvider::DecodeResult VideoSourceProvider::DecodeOneFrame(
        DecodedFrame& frame, uint64_t generation, std::stop_token token, ReadStats& stats, double skipThrough,
        double progressFrom)
    {
        IMFSourceReader* reader = m_reader.Reader();
        if (!reader) return DecodeResult::Failed;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;
        HRESULT hr = S_OK;
        double sampleSeconds = 0.0;
        double sampleDuration = 0.0;
        int reads = 0;
        const auto readStart = std::chrono::steady_clock::now();
        auto firstSampleAt = readStart;

        // Read until a sample is still showing at the target. Earlier ones are
        // already in the past: after a seek they lead up from the keyframe MF
        // lands on, and in playback they are frames the target has moved past.
        for (;;)
        {
            if (token.stop_requested() || m_generation.load() != generation)
                return DecodeResult::Superseded;
            sample = nullptr;
            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0, &streamIndex, &flags, &timestamp, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return DecodeResult::Failed;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return DecodeResult::EndOfStream;
            if (!sample) continue;   // a stream tick or format notice
            sampleSeconds = static_cast<double>(timestamp) / 10'000'000.0;
            if (reads++ == 0)
            {
                firstSampleAt = std::chrono::steady_clock::now();
                stats.firstSampleTime = sampleSeconds;
                stats.secondsToFirstSample = std::chrono::duration<double>(firstSampleAt - readStart).count();
            }
            if (sampleSeconds <= skipThrough + scTimeTolerance) continue;

            LONGLONG durationTicks = 0;
            sampleDuration = (SUCCEEDED(sample->GetSampleDuration(&durationTicks)) && durationTicks > 0)
                ? static_cast<double>(durationTicks) / 10'000'000.0 : Info().frameDuration;
            {
                std::lock_guard lock(m_stateMutex);
                if (m_generation.load() == generation) m_decodeHead = (std::max)(m_decodeHead, sampleSeconds);
            }
            if (sampleSeconds + sampleDuration > m_targetTime.load() + 1e-6) break;
            if (sampleSeconds >= progressFrom && SecondsSince(readStart) > scCatchUpFrameSeconds) break;
        }

        // Timed from the first sample, so a seek's own latency is not counted.
        if (reads > scMinTimedReads)
            m_seekModel.RecordDecodeRate(SecondsSince(firstSampleAt) / (reads - 1));
        frame.time = sampleSeconds;
        frame.duration = sampleDuration;

        return m_reader.CopySample(sample.get(), frame, m_zeroCopyAllowed && !m_zeroCopyFailed)
            ? DecodeResult::Frame : DecodeResult::Failed;
    }
}
