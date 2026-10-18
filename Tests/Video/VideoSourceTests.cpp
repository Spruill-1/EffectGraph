#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Graph/GraphEdits.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// Video sources.
namespace
{
    using namespace ShaderLab::Tests;

    // Video source: a frame-accurate seek, no re-seek once satisfied, and the
    // zero-copy (hardware decode) upload producing exactly what the CPU Lock2D
    // path does. On WARP there is no hardware decoder, so both providers take
    // the CPU path; the zero-copy half needs a hardware run.
    SL_TEST_CASE(TestVideoSourceSeekAndZeroCopy, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: frame-accurate seek + zero-copy upload ===\n");
        struct Clip { const wchar_t* file; double frame; const char* tag; };
        const Clip clips[] = {
            { L"video_hdr10_hevc_30f.mp4", 17.5, "HEVC Main10 (P010)" },
            { L"video_sdr_h264_30f.mp4",   17.5, "H.264 High, B-frames (NV12)" },
        };
        auto readback = [&](ID3D11Texture2D* tex, std::vector<uint8_t>& out) -> bool
        {
            if (!tex) return false;
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (FAILED(g_d3dDevice->CreateTexture2D(&d, nullptr, staging.put()))) return false;
            g_d3dContext->CopyResource(staging.get(), tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            const size_t row = static_cast<size_t>(d.Width) * 8;   // RGBA16F
            out.resize(row * d.Height);
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(out.data() + y * row, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, row);
            g_d3dContext->Unmap(staging.get(), 0);
            return true;
        };
        for (const auto& clip : clips)
        {
            const std::wstring path = FindFixture(clip.file);
            if (path.empty())
            {
                // Generated, not checked in -- and CI has no ffmpeg. Neither a
                // pass nor a fail: say so loudly instead.
                SkipMissingFixture(clip.file);
                continue;
            }

            ShaderLab::Effects::VideoSourceProvider zc, cpu;
            cpu.SetZeroCopyEnabled(false);
            const bool opened =
                zc.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get()) &&
                cpu.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            TEST("Video_Opens", opened);
            if (!opened) continue;

            const double fps = zc.FrameRate() > 0 ? zc.FrameRate() : 30.0;
            const double target = clip.frame / fps;   // mid-frame, far from the only keyframe
            // Open decodes the first frame synchronously; consume it so the
            // upload below is the seek's own frame.
            zc.UploadIfReady(g_dc.get());
            cpu.UploadIfReady(g_dc.get());
            const uint64_t dz = zc.DecodeCount(), dcpu = cpu.DecodeCount();
            bool uploadedZc = false, uploadedCpu = false;
            zc.Seek(target);
            cpu.Seek(target);
            for (int i = 0; i < 600 && !(uploadedZc && uploadedCpu); ++i)
            {
                if (!uploadedZc && zc.DecodeCount() > dz)    uploadedZc  = zc.UploadIfReady(g_dc.get());
                if (!uploadedCpu && cpu.DecodeCount() > dcpu) uploadedCpu = cpu.UploadIfReady(g_dc.get());
                if (!(uploadedZc && uploadedCpu)) ::Sleep(5);
            }
            TEST("Video_SeekDeliversAFrame", uploadedZc && uploadedCpu);
            if (!(uploadedZc && uploadedCpu))
                printf("    (debug) decodes zc %llu->%llu cpu %llu->%llu, err '%ls'\n",
                    (unsigned long long)dz, (unsigned long long)zc.DecodeCount(),
                    (unsigned long long)dcpu, (unsigned long long)cpu.DecodeCount(),
                    zc.LastError().c_str());

            // The frame shown must be the one covering the target, not the
            // keyframe MF lands on.
            const double pos = zc.CurrentPosition();
            const double expected = std::floor(clip.frame) / fps;
            printf("  [info] %s: %.2f fps, target %.4f s -> frame at %.4f s (expected %.4f), zero-copy %s\n",
                clip.tag, fps, target, pos, expected, zc.LastUploadWasZeroCopy() ? "yes" : "no");
            TEST("Video_SeekIsFrameAccurate", std::abs(pos - expected) < 0.25 / fps);
            TEST("Video_SeekTargetRemembered", zc.LastSeekTarget() == target);

            std::vector<uint8_t> a, b;
            const bool readA = readback(zc.OutputTexture(), a);
            const bool readB = readback(cpu.OutputTexture(), b);
            TEST("Video_ZeroCopyMatchesCpuPath", readA && readB && a == b);
            if (readA && readB && a != b && a.size() == b.size())
            {
                const size_t row = static_cast<size_t>(zc.FrameWidth()) * 8;
                size_t firstRow = SIZE_MAX, rowsDiff = 0;
                for (size_t y = 0; y * row < a.size(); ++y)
                    if (std::memcmp(a.data() + y * row, b.data() + y * row, row) != 0)
                    { ++rowsDiff; if (firstRow == SIZE_MAX) firstRow = y; }
                printf("    (debug) %ux%u: %zu rows differ, first %zu\n",
                    zc.FrameWidth(), zc.FrameHeight(), rowsDiff, firstRow);
            }
            // Informational: decode + upload wall time per frame, both paths.
            auto timeFrames = [&](ShaderLab::Effects::VideoSourceProvider& p, int frames) -> double
            {
                p.Seek(0.0);
                auto t0 = std::chrono::steady_clock::now();
                int got = 0;
                while (got < frames &&
                       std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
                {
                    if (p.UploadIfReady(g_dc.get())) { ++got; p.RequestNextFrame(); }
                    else std::this_thread::yield();
                }
                g_d3dContext->Flush();
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                return got ? ms / got : 0.0;
            };
            const double msZc = timeFrames(zc, 20);
            const double msCpu = timeFrames(cpu, 20);
            printf("  [info] %s %ux%u: %.2f ms/frame zero-copy vs %.2f ms/frame CPU path\n",
                clip.tag, zc.FrameWidth(), zc.FrameHeight(), msZc, msCpu);
            zc.Close();
            cpu.Close();
        }
    }

    // Video frame conversion must survive another D2D context drawing on the
    // same D3D11 device, as the GUI's UI thread does. The output is cleared to
    // transparent black before each upload, so a lost dispatch reads as alpha 0.
    SL_TEST_CASE(TestVideoUploadUnderD2DContention, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: frame upload under concurrent D2D drawing ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }
        winrt::com_ptr<ID2D1Factory7> factory;
        winrt::com_ptr<ID2D1Device6> device;
        winrt::com_ptr<ID2D1DeviceContext5> dcWorker, dcUi;
        bool ok = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
            __uuidof(ID2D1Factory7), nullptr, factory.put_void()));
        ok = ok && SUCCEEDED(factory->CreateDevice(g_d3dDevice.as<IDXGIDevice>().get(),
            reinterpret_cast<ID2D1Device**>(device.put())));
        ok = ok && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dcWorker.put())));
        ok = ok && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dcUi.put())));
        TEST("VideoContention_Setup", ok);
        if (!ok) return;

        ShaderLab::Effects::VideoSourceProvider video;
        video.SetLoop(true);
        const bool opened = video.Open(path, dcWorker.get(), g_d3dDevice.get(), g_d3dContext.get());
        TEST("VideoContention_Opens", opened);
        if (!opened) return;

        // Stand-in for the UI thread: draw a blurred flood into its own target
        // as fast as it can until told to stop.
        std::atomic<bool> stop{ false };
        std::atomic<uint32_t> uiDraws{ 0 }, uiEndDrawFailures{ 0 };
        std::thread uiThread([&] {
            winrt::com_ptr<ID2D1Bitmap1> target;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            if (FAILED(dcUi->CreateBitmap({ 512, 512 }, nullptr, 0, targetProps, target.put()))) return;
            winrt::com_ptr<ID2D1Effect> flood, blur;
            dcUi->CreateEffect(CLSID_D2D1Flood, flood.put());
            dcUi->CreateEffect(CLSID_D2D1GaussianBlur, blur.put());
            if (!flood || !blur) return;
            blur->SetInputEffect(0, flood.get());
            dcUi->SetTarget(target.get());
            for (uint32_t frame = 0; !stop.load(); ++frame)
            {
                flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1::Vector4F(0.2f, 0.5f, float(frame % 7) / 7.0f, 1.0f));
                dcUi->BeginDraw();
                dcUi->DrawImage(blur.get());
                if (FAILED(dcUi->EndDraw())) ++uiEndDrawFailures;
                ++uiDraws;
            }
            dcUi->SetTarget(nullptr);
        });

        // Stand-in for the render worker: clear, upload the next frame, read
        // back the centre texel.
        winrt::com_ptr<ID3D11UnorderedAccessView> clearUav;
        g_d3dDevice->CreateUnorderedAccessView(video.OutputTexture(), nullptr, clearUav.put());
        D3D11_TEXTURE2D_DESC texelDesc{};
        video.OutputTexture()->GetDesc(&texelDesc);
        texelDesc.Width = texelDesc.Height = 1; texelDesc.Usage = D3D11_USAGE_STAGING; texelDesc.BindFlags = 0;
        texelDesc.MiscFlags = 0; texelDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> texel;
        g_d3dDevice->CreateTexture2D(&texelDesc, nullptr, texel.put());
        const UINT centerX = video.FrameWidth() / 2, centerY = video.FrameHeight() / 2;

        int uploads = 0, lost = 0;
        const auto start = std::chrono::steady_clock::now();
        while (uploads < 400 && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
        {
            const float zero[4] = { 0, 0, 0, 0 };
            g_d3dContext->ClearUnorderedAccessViewFloat(clearUav.get(), zero);
            video.RequestNextFrame();
            bool uploaded = false;
            for (int attempt = 0; attempt < 400 && !uploaded; ++attempt)
            {
                uploaded = video.UploadIfReady(dcWorker.get());
                if (!uploaded) ::Sleep(1);
            }
            if (!uploaded) continue;
            ++uploads;
            D3D11_BOX box = { centerX, centerY, 0, centerX + 1, centerY + 1, 1 };
            g_d3dContext->CopySubresourceRegion(texel.get(), 0, 0, 0, 0, video.OutputTexture(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_d3dContext->Map(texel.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                const uint16_t alphaHalf = static_cast<const uint16_t*>(mapped.pData)[3];
                g_d3dContext->Unmap(texel.get(), 0);
                if (alphaHalf != 0x3C00) ++lost;   // 1.0 in FP16
            }
        }
        stop = true;
        uiThread.join();
        video.Close();
        printf("  [info] %d uploads while the UI context drew %u frames: %d lost (output left at alpha 0); "
               "UI EndDraw failures %u\n", uploads, uiDraws.load(), lost, uiEndDrawFailures.load());
        // WARP rasterises on the CPU, so the drawing thread starves the uploads
        // and the race rarely shows; there the test only has to show it ran.
        const int minUploads = g_useWarp ? 20 : 100;
        if (g_useWarp)
            printf("  [info] WARP: contention evidence is weak here; run with --adapter default for the real check\n");
        TEST("VideoContention_EnoughWork", uploads >= minUploads && uiDraws.load() >= 100);
        TEST("VideoContention_NoLostConversion", lost == 0);
        TEST("VideoContention_NoUiEndDrawFailures", uiEndDrawFailures.load() == 0);
    }

    // Video FrameTime: the analysis field matches the frame actually uploaded
    // on the same tick, resets on Close, and a negative looped Time wraps to
    // the end of the clip instead of clamping to the first frame.
    SL_TEST_CASE(TestVideoFrameTimeReporting, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: FrameTime reporting ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }

        // Provider: -1 before the first upload and again after Close.
        {
            ShaderLab::Effects::VideoSourceProvider video;
            const bool opened = video.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            TEST("VideoFrameTime_Opens", opened);
            if (!opened) return;
            const double beforeUpload = video.UploadedFrameTime();
            const bool uploaded = video.UploadIfReady(g_dc.get());
            const double firstFrame = video.UploadedFrameTime();
            const double firstDuration = video.UploadedFrameDuration();
            printf("  [info] first upload %s: frame at %.4f s, shown for %.4f s\n",
                   uploaded ? "ok" : "failed", firstFrame, firstDuration);
            TEST("VideoFrameTime_UnsetBeforeFirstUpload", beforeUpload == -1.0);
            TEST("VideoFrameTime_FirstUploadIsFirstFrame", uploaded && firstFrame >= 0.0 && firstFrame == video.FirstFrameTime());
            TEST("VideoFrameTime_DurationIsOneFrame", std::abs(firstDuration - 1.0 / 30.0) < 1e-3);
            video.Close();
            TEST("VideoFrameTime_ResetOnClose", video.UploadedFrameTime() == -1.0 && video.UploadedFrameDuration() == 0.0);
        }

        // Factory: the FrameTime field written on a tick is the frame uploaded on that tick.
        using ShaderLab::Effects::SourceNodeFactory;
        SourceNodeFactory factory;
        ShaderLab::Graph::GraphModel graph;
        const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
        auto nodes = graph.MutableNodes();
        auto* videoNode = graph.FindNode(videoId);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        auto* provider = factory.VideoProviderForRenderThread(videoId);
        TEST("VideoFrameTime_FactoryOpens", provider != nullptr);
        if (!provider) return;
        auto frameTimeField = [&]
        {
            for (const auto& field : graph.FindNode(videoId)->analysisOutput.fields)
                if (field.name == L"FrameTime") return static_cast<double>(field.components[0]);
            return -2.0;
        };
        // Tick until the uploaded frame is the one for `seconds`; every tick
        // must report exactly the uploaded frame.
        auto tickUntil = [&](float seconds, double expectedFrame, bool& fieldAlwaysMatched)
        {
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(videoId), L"Time", seconds);
            for (int i = 0; i < 500; ++i)
            {
                factory.TickAndUploadVideos(nodes, g_dc.get());
                if (std::abs(frameTimeField() - static_cast<float>(provider->UploadedFrameTime())) > 1e-6)
                    fieldAlwaysMatched = false;
                if (std::abs(provider->UploadedFrameTime() - expectedFrame) < 1e-3) return true;
                ::Sleep(10);
            }
            return false;
        };
        bool fieldMatched = true;
        const bool reachedMid = tickUntil(17.5f / 30.0f, 17.0 / 30.0, fieldMatched);
        TEST("VideoFrameTime_FactoryReachesSeekTarget", reachedMid);
        TEST("VideoFrameTime_FieldMatchesUploadSameTick", fieldMatched);
        // Half a frame before 0, looped, is inside the last frame.
        const double lastFrame = std::floor(provider->Duration() * 30.0 - 0.5) / 30.0;
        bool wrapMatched = true;
        const bool reachedWrap = tickUntil(-0.5f / 30.0f, lastFrame, wrapMatched);
        printf("  [info] Time -0.5 frame, looped: frame at %.4f s (expected %.4f s)\n",
               provider->UploadedFrameTime(), lastFrame);
        TEST("VideoFrameTime_NegativeLoopedTimeWraps", reachedWrap && wrapMatched);
    }

    // A video node re-pointed at a file that does not open drops the old
    // provider's bitmap instead of keeping a pointer to it. The factory is
    // owned by this thread, so a Debug build also checks every call's thread.
    SL_TEST_CASE(TestVideoPathChangeToMissingFile, Tag::Gpu | Tag::Video)
    {
        printf("\n=== Video source: path changed to a missing file ===\n");
        const std::wstring path = FindFixture(L"video_sdr_h264_30f.mp4");
        if (path.empty())
        {
            SkipMissingFixture(L"video_sdr_h264_30f.mp4");
            return;
        }
        using ShaderLab::Effects::SourceNodeFactory;
        SourceNodeFactory factory;
        factory.SetOwnerThread(std::this_thread::get_id());
        ShaderLab::Graph::GraphModel graph;
        const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
        auto* videoNode = graph.FindNode(videoId);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        factory.TickAndUploadVideos(graph.MutableNodes(), g_dc.get());
        const bool opened = videoNode->cachedOutput != nullptr;

        ShaderLab::Graph::SetNodeMedia(*videoNode, (std::filesystem::temp_directory_path() / L"ShaderLabAbsentClip.mp4").wstring());
        ShaderLab::Graph::MarkNodeChanged(*videoNode);
        factory.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        printf("  [info] after re-point: '%ls'\n", videoNode->runtimeError.c_str());
        TEST("VideoSource_FirstClipShowsAFrame", opened);
        TEST("VideoSource_FailedReopenDropsOutput",
             videoNode->cachedOutput == nullptr && videoNode->runtimeError.starts_with(L"Failed to open video:") &&
             !factory.HasVideoProvider(videoId));
        factory.ReleaseCache();
        factory.SetOwnerThread({});
    }
}

namespace ShaderLab::Tests
{
    // Decoder surface pool probe: how many hardware-decoded samples a caller
    // can hold before ReadSample blocks waiting for a surface to come back.
    // Each read runs on a helper thread with a timeout; on a stall the held
    // samples are released, which lets the blocked read return.
    namespace
    {
        struct SurfacePoolProbe
        {
            int held{ 0 };          // samples held when the next read stalled
            bool stalled{ false };
            bool hardware{ false }; // every sample was a DXGI surface
            std::string error;
        };

        SurfacePoolProbe ProbeSurfacePool(const std::wstring& path, ID3D11Device* device, UINT extraOutputSamples)
        {
            SurfacePoolProbe probe;
            UINT resetToken = 0;
            winrt::com_ptr<IMFDXGIDeviceManager> manager;
            if (FAILED(MFCreateDXGIDeviceManager(&resetToken, manager.put())) ||
                FAILED(manager->ResetDevice(device, resetToken)))
            {
                probe.error = "DXGI device manager";
                return probe;
            }
            winrt::com_ptr<IMFAttributes> attributes;
            MFCreateAttributes(attributes.put(), 2);
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.get());
            winrt::com_ptr<IMFSourceReader> reader;
            if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put())))
            {
                probe.error = "source reader";
                return probe;
            }
            const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            reader->SetStreamSelection(stream, TRUE);
            winrt::com_ptr<IMFMediaType> outputType;
            MFCreateMediaType(outputType.put());
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            if (FAILED(reader->SetCurrentMediaType(stream, nullptr, outputType.get())))
            {
                probe.error = "NV12 output";
                return probe;
            }
            if (extraOutputSamples > 0)
            {
                winrt::com_ptr<IMFSourceReaderEx> readerEx;
                winrt::com_ptr<IMFTransform> decoder;
                winrt::com_ptr<IMFAttributes> outputAttributes;
                GUID category{};
                if (!reader.try_as(readerEx) ||
                    FAILED(readerEx->GetTransformForStream(stream, 0, &category, decoder.put())) ||
                    FAILED(decoder->GetOutputStreamAttributes(0, outputAttributes.put())) ||
                    FAILED(outputAttributes->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, extraOutputSamples)))
                {
                    probe.error = "MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT not settable";
                    return probe;
                }
            }

            constexpr int cMaxHeld = 64;
            probe.hardware = true;
            std::vector<winrt::com_ptr<IMFSample>> held;
            for (int i = 0; i < cMaxHeld; ++i)
            {
                winrt::com_ptr<IMFSample> sample;
                HRESULT readResult = E_FAIL;
                DWORD flags = 0;
                std::mutex mutex;
                std::condition_variable doneSignal;
                bool done = false;
                std::thread readThread([&]
                {
                    DWORD actualStream = 0;
                    LONGLONG timestamp = 0;
                    winrt::com_ptr<IMFSample> result;
                    const HRESULT hr = reader->ReadSample(stream, 0, &actualStream, &flags, &timestamp, result.put());
                    std::lock_guard lock(mutex);
                    readResult = hr;
                    sample = std::move(result);
                    done = true;
                    doneSignal.notify_one();
                });
                bool finished;
                {
                    std::unique_lock lock(mutex);
                    finished = doneSignal.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
                }
                if (!finished)
                {
                    probe.stalled = true;
                    probe.held = static_cast<int>(held.size());
                    held.clear();
                    readThread.join();
                    return probe;
                }
                readThread.join();
                if (FAILED(readResult) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample) break;
                winrt::com_ptr<IMFMediaBuffer> buffer;
                winrt::com_ptr<IMFDXGIBuffer> dxgiBuffer;
                if (FAILED(sample->GetBufferByIndex(0, buffer.put())) || !buffer.try_as(dxgiBuffer))
                    probe.hardware = false;
                held.push_back(std::move(sample));
            }
            probe.held = static_cast<int>(held.size());
            return probe;
        }
    }

    void TestVideoDecoderSurfacePool(ID3D11Device* device)
    {
        std::printf("\n=== Video decoder surface pool ===\n");
        bool anyRan = false;
        int smallestPool = 1000;
        for (const wchar_t* clip : { L"video_index_h264_180p60.mp4", L"video_index_h264_720p60.mp4",
                                     L"video_sdr_h264_30f.mp4", L"video_hdr10_hevc_30f.mp4" })
        {
            const std::wstring path = FindFixture(clip);
            if (path.empty()) continue;
            for (UINT extra : { 0u, 8u })
            {
                const auto probe = ProbeSurfacePool(path, device, extra);
                if (!probe.error.empty())
                {
                    std::printf("  [info] %-28ls extra %u: %s\n", clip, extra, probe.error.c_str());
                    continue;
                }
                anyRan = true;
                if (probe.stalled) smallestPool = (std::min)(smallestPool, probe.held);
                std::printf("  [info] %-28ls MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT %u: %s after holding %d samples (%s)\n",
                    clip, extra, probe.stalled ? "ReadSample stalls" : "no stall", probe.held,
                    probe.hardware ? "DXGI surfaces" : "not all DXGI surfaces");
            }
        }
        TEST("VideoSurfacePool_ProbeRan", anyRan);
        // The provider holds its queue plus the frame being converted.
        std::printf("  [info] provider holds at most %zu samples; smallest pool measured %d\n",
            Effects::VideoSourceProvider::DecodeAheadFrames() + 1, smallestPool);
        TEST("VideoSurfacePool_QueueFitsPool",
             static_cast<int>(Effects::VideoSourceProvider::DecodeAheadFrames()) + 1 < smallestPool);
    }

    namespace
    {
        SL_REGISTER_TEST(TestVideoDecoderSurfacePool, Tag::Gpu | Tag::Video,
            [] { TestVideoDecoderSurfacePool(g_d3dDevice.get()); });
    }
}
