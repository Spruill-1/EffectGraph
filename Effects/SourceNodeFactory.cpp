#include "pch_engine.h"
#include "SourceNodeFactory.h"
#include "../Graph/SourceKind.h"

#include <cassert>

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    namespace
    {
        constexpr std::wstring_view cLiveLostPrefix = L"Live source lost, retrying: ";
        constexpr std::wstring_view cVideoUploadErrorPrefix = L"Video upload failed: ";
        constexpr auto cLiveRetryInterval = std::chrono::seconds(1);

        void ReportVideoUploadFailure(Graph::EffectNode* node, uint32_t nodeId, const std::wstring& reason)
        {
            std::fwprintf(stderr, L"[ShaderLab] node %u video upload failed: %s\n", nodeId, reason.c_str());
            if (node)
                node->runtimeError = std::wstring(cVideoUploadErrorPrefix) + reason;
        }

        double VideoTimeProperty(const Graph::EffectNode& node)
        {
            auto it = node.properties.find(L"Time");
            if (it != node.properties.end())
                if (auto* f = std::get_if<float>(&it->second))
                    return static_cast<double>(*f);
            return 0.0;
        }

        // Replaces the node's analysis fields with `names`, all Float, set to
        // `values` (zero where `values` is shorter).
        void WriteFloatFields(Graph::EffectNode& node, std::span<const wchar_t* const> names,
                              std::initializer_list<double> values = {})
        {
            node.analysisOutput.type = Graph::AnalysisOutputType::Typed;
            node.analysisOutput.fields.clear();
            node.analysisOutput.fields.reserve(names.size());
            auto value = values.begin();
            for (const wchar_t* name : names)
            {
                Graph::AnalysisFieldValue field;
                field.name = name;
                field.type = Graph::AnalysisFieldType::Float;
                if (value != values.end())
                    field.components[0] = static_cast<float>(*value++);
                node.analysisOutput.fields.push_back(std::move(field));
            }
        }

        void WriteVideoFields(Graph::EffectNode& node, const VideoSourceProvider& provider, double position)
        {
            WriteFloatFields(node, SourceNodeFactory::scVideoFields, {
                provider.Duration(),
                position,
                static_cast<double>(provider.DecodeCount()),
                static_cast<double>(provider.UploadSuccesses()),
                static_cast<double>(provider.UploadAttempts()),
                provider.UploadedFrameTime(),   // the frame cachedOutput holds
                static_cast<double>(static_cast<int>(provider.GetOutputFormat())),   // 0 RGB32, 1 NV12, 2 P010
            });
        }

        void WriteLiveFields(Graph::EffectNode& node, const ILiveSource& source)
        {
            WriteFloatFields(node, SourceNodeFactory::scLiveFields, {
                static_cast<double>(source.Width()),
                static_cast<double>(source.Height()),
                static_cast<double>(source.FrameCount()),
                source.DeliversFP16() ? 1.0 : 0.0,
            });
        }

        std::wstring LiveOpenFailure(const LiveSourceKind& kind, const std::wstring& reason)
        {
            return L"Failed to open " + kind.displayName + L": " + reason;
        }

        std::wstring PrepareFailure(std::wstring_view detail)
        {
            std::wstring message = L"Source preparation failed";
            if (!detail.empty())
                message += std::wstring(L": ") + std::wstring(detail);
            return message;
        }

        template <typename T>
        bool ReadProperty(const Graph::EffectNode& node, const wchar_t* name, T& value)
        {
            auto it = node.properties.find(name);
            if (it == node.properties.end()) return false;
            if (auto* v = std::get_if<T>(&it->second)) { value = *v; return true; }
            return false;
        }

        // Nodes by id, built once per tick rather than scanned per provider.
        std::unordered_map<uint32_t, Graph::EffectNode*> NodesById(std::span<Graph::EffectNode> nodes)
        {
            std::unordered_map<uint32_t, Graph::EffectNode*> byId;
            byId.reserve(nodes.size());
            for (auto& node : nodes)
                byId.emplace(node.id, &node);
            return byId;
        }
    }

    std::optional<double> SourceNodeFactory::VideoTargetTime(const Graph::EffectNode& node, double duration)
    {
        double seconds = VideoTimeProperty(node);
        bool loop = true;
        ReadProperty(node, L"Loop", loop);
        if (duration <= 0.0) return (std::max)(seconds, 0.0);
        if (loop)
        {
            seconds = std::fmod(seconds, duration);
            if (seconds < 0.0) seconds += duration;
            return seconds;
        }
        if (seconds >= duration) return std::nullopt;
        return (std::max)(seconds, 0.0);
    }

    SourceNodeFactory::SourceNodeFactory() = default;

    void SourceNodeFactory::SetOwnerThread(std::thread::id owner)
    {
        m_ownerThread.store(owner, std::memory_order_release);
    }

    void SourceNodeFactory::AssertOwnerThread() const
    {
#ifndef NDEBUG
        const std::thread::id owner = m_ownerThread.load(std::memory_order_acquire);
        assert(owner == std::thread::id{} || owner == std::this_thread::get_id());
#endif
    }

    // -----------------------------------------------------------------------
    // Video file detection
    // -----------------------------------------------------------------------

    std::span<const std::wstring_view> SourceNodeFactory::VideoExtensions()
    {
        static constexpr std::wstring_view scExtensions[] = {
            L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".webm",
            L".m4v", L".ts", L".mts", L".flv", L".3gp"
        };
        return scExtensions;
    }

    bool SourceNodeFactory::IsVideoFile(const std::wstring& filePath)
    {
        auto ext = std::filesystem::path(filePath).extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        const auto extensions = VideoExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    // -----------------------------------------------------------------------
    // Node creation (static — no device needed yet)
    // -----------------------------------------------------------------------

    EffectNode SourceNodeFactory::CreateImageSourceNode(
        const std::wstring& filePath,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? std::filesystem::path(filePath).filename().wstring()
            : displayName;
        node.mediaPath = filePath;

        node.outputPins.push_back({ L"Image", 0 });
        return node;
    }

    EffectNode SourceNodeFactory::CreateFloodSourceNode(
        const Numerics::float4& color,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty() ? L"Flood Fill" : displayName;
        node.effectClsid = CLSID_D2D1Flood;
        node.properties[L"Color"] = color;
        node.outputPins.push_back({ L"Output", 0 });
        return node;
    }

    EffectNode SourceNodeFactory::CreateVideoSourceNode(
        const std::wstring& filePath,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? (L"\U0001F3AC " + std::filesystem::path(filePath).filename().wstring())
            : displayName;
        node.mediaPath = filePath;

        // Video-specific properties.
        node.properties[cVideoMarker] = true;
        node.properties[L"Loop"] = true;
        node.properties[L"Time"] = 0.0f;  // Clock-driven seek position (bound via data pin)

        node.outputPins.push_back({ L"Frame", 0 });

        // Every field exists before the first frame, so its data pins can
        // be bound at once.
        WriteFloatFields(node, scVideoFields);
        return node;
    }

    EffectNode SourceNodeFactory::CreateLiveSourceNode(
        const std::wstring& marker,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName;
        node.properties[marker] = true;
        node.outputPins.push_back({ L"Frame", 0 });
        WriteFloatFields(node, scLiveFields);
        return node;
    }

    // -----------------------------------------------------------------------
    // Source preparation (needs D2D device context)
    // -----------------------------------------------------------------------

    void SourceNodeFactory::PrepareSourceNode(
        EffectNode& node,
        ID2D1DeviceContext5* dc,
        ID3D11Device* d3dDevice,
        ID3D11DeviceContext* d3dContext)
    {
        AssertOwnerThread();
        if (!dc || node.type != NodeType::Source)
            return;

        try
        {
            switch (ClassifySource(node))
            {
            case SourceKind::Image: PrepareImage(node, dc); break;
            case SourceKind::Flood: PrepareFlood(node, dc); break;
            case SourceKind::Video: PrepareVideo(node, dc, d3dDevice, d3dContext); break;
            case SourceKind::Live:  PrepareLive(node, dc, d3dDevice); break;
            case SourceKind::None:  break;
            }
            return;
        }
        catch (const winrt::hresult_error& ex)
        {
            node.runtimeError = PrepareFailure(std::wstring_view(ex.message()));
        }
        catch (const std::exception& ex)
        {
            node.runtimeError = PrepareFailure(std::wstring_view(winrt::to_hstring(ex.what())));
        }
        catch (...)
        {
            node.runtimeError = PrepareFailure({});
        }
        // Clean, so a failing source neither stops the others nor keeps the
        // graph re-rendering.
        node.dirty = false;
    }

    void SourceNodeFactory::PrepareLive(EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice)
    {
        if (!d3dDevice)
            return;

        auto kind = FindLiveSourceKind(node);
        if (!kind)
        {
            node.runtimeError = L"No live source of this kind is registered in this program.";
            node.cachedOutput = nullptr;
            m_liveSources.erase(node.id);
            return;
        }

        // A node from a saved graph has no fields until now.
        if (node.analysisOutput.fields.empty())
            WriteFloatFields(node, scLiveFields);

        auto& entry = m_liveSources[node.id];
        if (entry.source && (entry.kind != kind || !entry.source->Matches(node)))
            entry = {};
        if (!entry.source)
        {
            m_liveRetryAt.erase(node.id);
            entry.kind = kind;

            winrt::Windows::Foundation::IInspectable target{ nullptr };
            if (auto it = m_liveTargets.find(node.id); it != m_liveTargets.end())
                target = it->second;
            std::wstring error;
            entry.source = kind->create(target, error);
            if (!entry.source)
            {
                node.runtimeError = error.empty() ? LiveOpenFailure(*kind, L"no source") : error;
                node.cachedOutput = nullptr;
                m_liveSources.erase(node.id);
                return;
            }
            // A failed Open keeps its source, so the tick retries it like a
            // lost one.
            if (!entry.source->Open(node, dc, d3dDevice))
            {
                entry.openFailed = true;
                node.runtimeError = LiveOpenFailure(*kind, entry.source->LastError());
                node.cachedOutput = nullptr;
                return;
            }
            entry.source->Describe(node);
            node.runtimeError.clear();
        }
        node.cachedOutput = entry.source->CurrentImage();
    }

    void SourceNodeFactory::PrepareVideo(EffectNode& node, ID2D1DeviceContext5* dc,
                                         ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    {
        const std::wstring videoPath = node.mediaPath.value_or(L"");
        if (videoPath.empty())
        {
            node.cachedOutput = nullptr;
            return;
        }

        // A node from a saved graph has no fields until now.
        if (node.analysisOutput.fields.empty())
            WriteFloatFields(node, scVideoFields);

        auto& provider = m_videoCache[node.id];

        // A new path re-opens; the old provider's bitmap goes with it.
        if (provider && provider->IsOpen() && provider->FilePath() != videoPath)
        {
            provider.reset();
            node.cachedOutput = nullptr;
        }

        if (!provider)
        {
            provider = std::make_unique<VideoSourceProvider>();
            OutputDebugStringW(std::format(L"[VideoSource] Opening: {}\n", videoPath).c_str());
            if (!provider->Open(videoPath, dc, d3dDevice, d3dContext))
            {
                node.runtimeError = L"Failed to open video: " + provider->LastError();
                node.cachedOutput = nullptr;
                OutputDebugStringW(std::format(L"[VideoSource] {}\n", node.runtimeError).c_str());
                m_videoCache.erase(node.id);
                return;
            }
            node.runtimeError.clear();
            OutputDebugStringW(std::format(L"[VideoSource] Opened: {}x{} @ {:.1f}fps, {:.1f}s, HDR={}\n",
                provider->FrameWidth(), provider->FrameHeight(),
                provider->FrameRate(), provider->Duration(),
                provider->IsHDR()).c_str());
        }

        bool loop = true;
        if (ReadProperty(node, L"Loop", loop))
            provider->SetLoop(loop);

        // Frames are uploaded by TickAndUploadVideos before evaluation; here
        // the node just points at the current image.
        auto pastEnd = m_videoPastEnd.find(node.id);
        if (pastEnd != m_videoPastEnd.end())
        {
            winrt::com_ptr<ID2D1Image> black;
            pastEnd->second->GetOutput(black.put());
            node.cachedOutput = black.get();
        }
        else
        {
            node.cachedOutput = provider->CurrentBitmap();
        }
    }

    SourceNodeFactory::ImageFileStamp SourceNodeFactory::StampOf(const std::wstring& path)
    {
        ImageFileStamp stamp;
        stamp.path = path;
        std::error_code ec;
        stamp.writeTime = std::filesystem::last_write_time(path, ec);
        if (ec) stamp.writeTime = {};
        stamp.size = std::filesystem::file_size(path, ec);
        if (ec) stamp.size = 0;
        return stamp;
    }

    void SourceNodeFactory::PrepareImage(EffectNode& node, ID2D1DeviceContext5* dc)
    {
        // No file chosen yet: nothing to decode, and nothing wrong.
        if (node.mediaPath->empty())
        {
            node.runtimeError.clear();
            node.cachedOutput = nullptr;
            node.dirty = false;
            return;
        }

        // Decode is keyed on the file, not on node.dirty: MarkAllDirty fires
        // on load, display-profile change and forced redraw, and none of
        // those may re-decode. Re-saving the file does.
        const auto stamp = StampOf(node.mediaPath.value());
        auto it = m_images.find(node.id);
        if (it != m_images.end() && it->second.bitmap && it->second.stamp == stamp)
        {
            node.cachedOutput = it->second.bitmap.get();
            node.dirty = false;
            return;
        }

        auto bitmap = m_imageLoader.LoadFromFile(stamp.path, dc);
        if (bitmap)
        {
            node.cachedOutput = bitmap.get();
            m_images[node.id] = { std::move(bitmap), stamp };
            node.runtimeError.clear();
            node.dirty = false;
        }
        else
        {
            // Not every file a WIC filter offers decodes (camera RAW, a
            // truncated file); runtimeError reaches the Properties panel
            // and graph_get_node.
            node.runtimeError = std::format(L"Failed to decode image: {} ({})", stamp.path, m_imageLoader.LastError());
        }
    }

    void SourceNodeFactory::PrepareFlood(EffectNode& node, ID2D1DeviceContext5* dc)
    {
        auto it = m_floodCache.find(node.id);
        winrt::com_ptr<ID2D1Effect> flood;

        if (it != m_floodCache.end())
        {
            flood = it->second;
        }
        else
        {
            HRESULT hr = dc->CreateEffect(CLSID_D2D1Flood, flood.put());
            if (FAILED(hr))
            {
                node.runtimeError = L"Failed to create the Flood effect: " + HResultMessage(hr);
                return;
            }
            m_floodCache[node.id] = flood;
        }

        Numerics::float4 color{};
        if (ReadProperty(node, L"Color", color))
            flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1_VECTOR_4F{ color.x, color.y, color.z, color.w });

        winrt::com_ptr<ID2D1Image> output;
        flood->GetOutput(output.put());
        node.cachedOutput = output.get();
        node.dirty = false;
    }

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    void SourceNodeFactory::ReleaseCache()
    {
        AssertOwnerThread();
        m_images.clear();
        m_floodCache.clear();
        m_videoCache.clear();
        m_videoPastEnd.clear();
        m_liveSources.clear();
        m_liveRetryAt.clear();
        m_videoSettleTimeouts.clear();
    }

    void SourceNodeFactory::PruneOrphans(std::span<const Graph::EffectNode> nodes)
    {
        AssertOwnerThread();
        std::unordered_map<uint32_t, const EffectNode*> sources;
        for (const auto& n : nodes)
            if (n.type == NodeType::Source) sources[n.id] = &n;
        auto isKind = [&](uint32_t id, SourceKind kind)
        {
            auto it = sources.find(id);
            return it != sources.end() && ClassifySource(*it->second) == kind;
        };
        std::erase_if(m_videoCache,  [&](const auto& kv) { return !isKind(kv.first, SourceKind::Video); });
        std::erase_if(m_liveSources, [&](const auto& kv) { return !isKind(kv.first, SourceKind::Live); });
        std::erase_if(m_liveRetryAt, [&](const auto& kv) { return !isKind(kv.first, SourceKind::Live); });
        // Plain per-id state: gone with the node. (m_liveTargets is left
        // alone -- a target can be set before its node exists.)
        auto gone = [&](const auto& kv) { return !sources.count(kv.first); };
        std::erase_if(m_images, gone);
        std::erase_if(m_floodCache, gone);
        std::erase_if(m_videoPastEnd, [&](const auto& kv) { return !isKind(kv.first, SourceKind::Video); });
    }

    bool SourceNodeFactory::TickAndUploadVideos(
        std::span<Graph::EffectNode> nodes,
        ID2D1DeviceContext5* dc)
    {
        PruneOrphans(nodes);
        bool anyNewFrame = false;
        const auto nodesById = NodesById(nodes);
        for (auto& [id, provider] : m_videoCache)
        {
            if (!provider || !provider->IsOpen()) continue;

            // The node, for its (possibly Clock-driven) Time.
            auto nodeIt = nodesById.find(id);
            Graph::EffectNode* nodePtr = nodeIt == nodesById.end() ? nullptr : nodeIt->second;

            // A failing decoder or upload is reported on its own node and
            // leaves the other videos ticking.
            try
            {
                double seekTime = 0.0;
                if (nodePtr)
                {
                    const auto target = VideoTargetTime(*nodePtr, provider->Duration());
                    if (target)
                        seekTime = *target;

                    // Past end of video with looping off → show black.
                    if (!target)
                    {
                        seekTime = VideoTimeProperty(*nodePtr);
                        auto [pastEnd, entered] = m_videoPastEnd.try_emplace(id);
                        if (entered)
                        {
                            // A null cachedOutput would stop the whole graph from
                            // evaluating, so black is an image: a flood cropped to
                            // the frame.
                            winrt::com_ptr<ID2D1Effect> flood, crop;
                            if (FAILED(dc->CreateEffect(CLSID_D2D1Flood, flood.put())) ||
                                FAILED(dc->CreateEffect(CLSID_D2D1Crop, crop.put())))
                            {
                                m_videoPastEnd.erase(pastEnd);
                                nodePtr->runtimeError = L"Could not create the black past-the-end image";
                                continue;
                            }
                            flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 1.0f });
                            crop->SetInputEffect(0, flood.get());
                            crop->SetValue(D2D1_CROP_PROP_RECT, D2D1_VECTOR_4F{ 0.0f, 0.0f,
                                static_cast<float>(provider->FrameWidth()), static_cast<float>(provider->FrameHeight()) });
                            pastEnd->second = crop;
                            nodePtr->dirty = true;
                        }
                        winrt::com_ptr<ID2D1Image> black;
                        pastEnd->second->GetOutput(black.put());
                        nodePtr->cachedOutput = black.get();
                        WriteVideoFields(*nodePtr, *provider, seekTime);
                        continue;
                    }
                }

                // Back from past the end: show the provider's frame again.
                if (m_videoPastEnd.erase(id) > 0 && nodePtr)
                {
                    nodePtr->cachedOutput = provider->CurrentBitmap();
                    nodePtr->dirty = true;
                }

                // Show the frame for Time, whether a Clock drives it or it is a
                // static value. The provider decodes forward (dropping frames a
                // slow tick has passed) or seeks, whichever is cheaper, and holds
                // the frame while Time stands still.
                provider->PlayTo(seekTime);

                if (provider->UploadIfReady(dc))
                {
                    anyNewFrame = true;
                    if (nodePtr)
                    {
                        nodePtr->cachedOutput = provider->CurrentBitmap();
                        nodePtr->dirty = true;
                        if (nodePtr->runtimeError.starts_with(cVideoUploadErrorPrefix))
                            nodePtr->runtimeError.clear();
                    }
                }

                // After the upload, so FrameTime matches cachedOutput.
                if (nodePtr)
                    WriteVideoFields(*nodePtr, *provider, provider->CurrentPosition());
            }
            catch (const winrt::hresult_error& ex)
            {
                ReportVideoUploadFailure(nodePtr, id, std::wstring(ex.message()));
            }
            catch (const std::exception& ex)
            {
                ReportVideoUploadFailure(nodePtr, id, std::wstring(winrt::to_hstring(ex.what())));
            }
        }
        return anyNewFrame;
    }

    bool SourceNodeFactory::TickLiveSources(
        std::span<Graph::EffectNode> nodes,
        ID2D1DeviceContext5* dc)
    {
        PruneOrphans(nodes);
        bool anyNewFrame = false;
        const auto now = std::chrono::steady_clock::now();
        const auto nodesById = NodesById(nodes);

        for (auto& [id, entry] : m_liveSources)
        {
            if (!entry.source) continue;
            ILiveSource& source = *entry.source;
            auto nodeIt = nodesById.find(id);
            Graph::EffectNode* node = nodeIt == nodesById.end() ? nullptr : nodeIt->second;

            CaptureResult result = CaptureResult::Failed;
            std::wstring reason;
            try
            {
                if (source.IsOpen())
                    result = source.NextFrame(dc);
                // No image at all is a failure even with no frame pending, so
                // the retry below still reaches Reopen.
                if (result == CaptureResult::NoFrame && !source.CurrentImage())
                    result = CaptureResult::Failed;

                if (result == CaptureResult::Failed)
                {
                    // The first failure starts the clock; each retry after
                    // the interval either reopens or restarts it.
                    auto [retry, first] = m_liveRetryAt.try_emplace(id, now + cLiveRetryInterval);
                    if (!first && now >= retry->second)
                    {
                        if (source.Reopen(dc))
                        {
                            m_liveRetryAt.erase(retry);
                            result = CaptureResult::NoFrame;
                            if (node && entry.openFailed)
                                source.Describe(*node);
                            entry.openFailed = false;
                        }
                        else
                        {
                            retry->second = now + cLiveRetryInterval;
                        }
                    }
                    if (result == CaptureResult::Failed)
                        reason = source.LastError();
                }
                else
                {
                    m_liveRetryAt.erase(id);
                }
            }
            catch (const winrt::hresult_error& ex)
            {
                result = CaptureResult::Failed;
                reason = std::wstring(ex.message());
            }
            catch (const std::exception& ex)
            {
                result = CaptureResult::Failed;
                reason = std::wstring(winrt::to_hstring(ex.what()));
            }

            if (result == CaptureResult::NewFrame)
                anyNewFrame = true;
            if (!node)
                continue;

            // Re-point the node every tick, not only on a new frame: a lost
            // or resized source releases the image the node still holds.
            ID2D1Image* current = source.CurrentImage();
            if (node->cachedOutput != current)
            {
                node->cachedOutput = current;
                node->dirty = true;
            }
            if (result == CaptureResult::NewFrame)
                node->dirty = true;

            if (result == CaptureResult::Failed || !current)
            {
                const std::wstring why = reason.empty() ? source.LastError() : reason;
                if (entry.openFailed)
                    node->runtimeError = LiveOpenFailure(*entry.kind, why);
                else
                    node->runtimeError = std::wstring(cLiveLostPrefix) + (why.empty() ? L"no image" : why);
            }
            else
            {
                node->runtimeError.clear();
            }
            if (source.IsOpen())
                WriteLiveFields(*node, source);
        }
        return anyNewFrame;
    }

    void SourceNodeFactory::SetLiveSourceTarget(
        uint32_t nodeId,
        winrt::Windows::Foundation::IInspectable const& target)
    {
        AssertOwnerThread();
        if (target)
            m_liveTargets[nodeId] = target;
        else
            m_liveTargets.erase(nodeId);
        m_liveSources.erase(nodeId);
        m_liveRetryAt.erase(nodeId);
    }

    VideoSourceProvider* SourceNodeFactory::VideoProviderForRenderThread(uint32_t nodeId)
    {
        AssertOwnerThread();
        auto it = m_videoCache.find(nodeId);
        return (it != m_videoCache.end()) ? it->second.get() : nullptr;
    }

    bool SourceNodeFactory::HasVideoProvider(uint32_t nodeId) const
    {
        AssertOwnerThread();
        auto it = m_videoCache.find(nodeId);
        return it != m_videoCache.end() && it->second;
    }

    std::vector<Graph::VideoSourceStatus> SourceNodeFactory::VideoSourceStatuses() const
    {
        AssertOwnerThread();
        std::vector<Graph::VideoSourceStatus> statuses;
        statuses.reserve(m_videoCache.size());
        for (const auto& [id, provider] : m_videoCache)
        {
            if (!provider) continue;
            Graph::VideoSourceStatus status;
            status.nodeId = id;
            status.open = provider->IsOpen();
            status.loop = provider->IsLooping();
            status.width = provider->FrameWidth();
            status.height = provider->FrameHeight();
            status.frameRate = provider->FrameRate();
            status.duration = provider->Duration();
            status.position = provider->CurrentPosition();
            status.uploads = provider->UploadSuccesses();
            statuses.push_back(status);
        }
        return statuses;
    }

    bool SourceNodeFactory::VideoSettleTimedOut(uint32_t nodeId, const std::wstring& path, double time) const
    {
        AssertOwnerThread();
        return m_videoSettleTimeouts.count({ nodeId, path, time }) > 0;
    }

    void SourceNodeFactory::RecordVideoSettleTimeout(uint32_t nodeId, const std::wstring& path, double time)
    {
        AssertOwnerThread();
        m_videoSettleTimeouts.insert({ nodeId, path, time });
    }

    void SourceNodeFactory::ForgetVideoSettleTimeouts()
    {
        AssertOwnerThread();
        m_videoSettleTimeouts.clear();
    }
}
