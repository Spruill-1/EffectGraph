#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"
#include "../Graph/VideoSourceStatus.h"
#include <span>
#include "ImageLoader.h"
#include "VideoSourceProvider.h"
#include "LiveSource.h"

namespace ShaderLab::Effects
{
    // Builds Source nodes, and keeps the per-node runtime that feeds them:
    // one decoded image, flood effect, video provider or live source per node
    // id, made by the Prepare function for the node's SourceKind.
    //
    // Threading: the instance belongs to the thread that renders with it. A
    // host whose render thread differs from the thread that created it names
    // that thread with SetOwnerThread; debug builds then assert every call
    // comes from it. A UI reads video state from a copy the render thread
    // publishes (VideoSourceStatuses), never from here.
    //
    // Colour: every image a Source produces is linear scRGB (1.0 = 80 nits).
    //
    // Errors: no member throws. A Source that cannot produce its image gets
    // the reason in node.runtimeError.
    class SourceNodeFactory
    {
    public:
        SourceNodeFactory();
        SourceNodeFactory(const SourceNodeFactory&) = delete;
        SourceNodeFactory& operator=(const SourceNodeFactory&) = delete;

        // The analysis fields a video Source publishes, in order. Every one is
        // present from creation and written on every tick, past the end too.
        static constexpr std::array<const wchar_t*, 7> scVideoFields = {
            L"Duration", L"Position", L"Decodes", L"Uploads", L"UploadAttempts", L"FrameTime", L"Format" };
        // The analysis fields a live Source publishes. FP16 is 1 when the
        // source delivers FP16 and 0 when it fell back to 8-bit.
        static constexpr std::array<const wchar_t*, 4> scLiveFields = {
            L"Width", L"Height", L"Frames", L"FP16" };

        // Create a Source node configured for image file loading.
        static Graph::EffectNode CreateImageSourceNode(
            const std::wstring& filePath,
            const std::wstring& displayName = L"");

        // Create a Source node configured as a Flood (solid color) fill.
        static Graph::EffectNode CreateFloodSourceNode(
            const winrt::Windows::Foundation::Numerics::float4& color,
            const std::wstring& displayName = L"");

        // Create a Source node configured for video file playback.
        static Graph::EffectNode CreateVideoSourceNode(
            const std::wstring& filePath,
            const std::wstring& displayName = L"");

        // Create a Source node of the live source kind with `marker`, its
        // fields seeded. The kind's own properties are the host's to add.
        static Graph::EffectNode CreateLiveSourceNode(
            const std::wstring& marker,
            const std::wstring& displayName);

        // Returns true if the file extension (any case) is one of VideoExtensions.
        static bool IsVideoFile(const std::wstring& filePath);

        // The video file extensions, lowercase with the dot: what IsVideoFile
        // accepts and what the video file pickers offer.
        static std::span<const std::wstring_view> VideoExtensions();

        // The time a video node shows for its Time property: looped into
        // [0, duration), or clamped at 0 without looping. Nullopt past the end
        // of a non-looping video, which shows black. A duration <= 0 (not yet
        // known) applies no wrap.
        static std::optional<double> VideoTargetTime(const Graph::EffectNode& node, double duration);

        // Point a Source node at its image: decodes the image file, creates
        // the Flood effect, or opens the video provider or live source, by the
        // node's SourceKind. A failure lands on node.runtimeError; one that
        // throws also leaves the node clean.
        void PrepareSourceNode(
            Graph::EffectNode& node,
            ID2D1DeviceContext5* dc,
            ID3D11Device* d3dDevice = nullptr,
            ID3D11DeviceContext* d3dContext = nullptr);

        // Releases everything made on the device: bitmaps, flood effects and
        // providers. Live-source targets are kept, so a picked capture
        // reopens on the next device by itself.
        void ReleaseCache();

        // Show each video's frame for its Time property (Clock-bound or
        // static) and upload it. Marks nodes dirty only when a new frame is
        // uploaded. Call before checking HasDirtyNodes. True if any new frame
        // was uploaded.
        bool TickAndUploadVideos(
            std::span<Graph::EffectNode> nodes,
            ID2D1DeviceContext5* dc);

        // Take each live source's next frame. A lost source reports the
        // reason on its node and is reopened once a second until it returns.
        // True if any source produced a new frame.
        bool TickLiveSources(
            std::span<Graph::EffectNode> nodes,
            ID2D1DeviceContext5* dc);

        // Hands the live source of node `nodeId` the object it opens on (a
        // picked window, say), or clears it with null. The source is
        // recreated on the next prepare. May come before the node exists.
        void SetLiveSourceTarget(
            uint32_t nodeId,
            winrt::Windows::Foundation::IInspectable const& target);

        // The video provider for a node. Render thread only: the worker
        // creates and destroys providers every tick. The UI reads
        // VideoSourceStatuses through the published snapshot instead.
        VideoSourceProvider* VideoProviderForRenderThread(uint32_t nodeId);
        bool HasVideoProvider(uint32_t nodeId) const;

        // Value copy of every video provider's playback state, for the snapshot.
        std::vector<Graph::VideoSourceStatus> VideoSourceStatuses() const;

        // Drop every cached provider / bitmap whose node is gone or is no
        // longer that kind of source, so a deleted capture stops capturing, a
        // deleted video stops decoding, and a node id reused by a loaded
        // graph starts fresh. Runs at the top of each per-tick loop, which
        // covers every removal path.
        void PruneOrphans(std::span<const Graph::EffectNode> nodes);

        // Videos that missed a settle timeout (SettleVideoSources) at a time,
        // so they are not waited on again there. Forgotten on ReleaseCache
        // and when a graph is loaded or cleared.
        bool VideoSettleTimedOut(uint32_t nodeId, const std::wstring& path, double time) const;
        void RecordVideoSettleTimeout(uint32_t nodeId, const std::wstring& path, double time);
        void ForgetVideoSettleTimeouts();

        // The thread every call must come from, checked in debug builds; a
        // default id turns the check off.
        void SetOwnerThread(std::thread::id owner);

    private:
        void AssertOwnerThread() const;

        void PrepareImage(Graph::EffectNode& node, ID2D1DeviceContext5* dc);
        void PrepareFlood(Graph::EffectNode& node, ID2D1DeviceContext5* dc);
        void PrepareVideo(Graph::EffectNode& node, ID2D1DeviceContext5* dc,
                          ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);
        void PrepareLive(Graph::EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice);

        // Which file a decoded image came from: the path, and the file's
        // write time and size when it was decoded.
        struct ImageFileStamp
        {
            std::wstring path;
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size{ 0 };
            bool operator==(const ImageFileStamp&) const = default;
        };
        static ImageFileStamp StampOf(const std::wstring& path);

        struct DecodedImage
        {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            ImageFileStamp stamp;
        };

        ImageLoader m_imageLoader;

        // Decoded images: nodeId -> bitmap and the file it came from. The
        // decode is redone when the file changes, not when the node is dirty.
        std::unordered_map<uint32_t, DecodedImage> m_images;

        // Cached flood effects: nodeId → flood effect.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_floodCache;

        // Cached video providers: nodeId → video provider.
        std::unordered_map<uint32_t, std::unique_ptr<VideoSourceProvider>> m_videoCache;

        // Opaque black at the video's size, shown instead of the provider's
        // bitmap while a non-looping video's Time is past its end: nodeId →
        // crop of a black flood.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_videoPastEnd;

        // A node's live source and the kind that made it.
        struct LiveEntry
        {
            std::shared_ptr<const LiveSourceKind> kind;
            std::unique_ptr<ILiveSource> source;
            // The node's error is this source's open failure, kept until a
            // reopen succeeds.
            bool openFailed{ false };
        };
        std::unordered_map<uint32_t, LiveEntry> m_liveSources;

        // What each live source opens on: nodeId -> target. Not a device
        // resource, so ReleaseCache keeps it.
        std::unordered_map<uint32_t, winrt::Windows::Foundation::IInspectable> m_liveTargets;

        // When a failed live source is next reopened: nodeId -> time.
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_liveRetryAt;

        std::set<std::tuple<uint32_t, std::wstring, double>> m_videoSettleTimeouts;

        std::atomic<std::thread::id> m_ownerThread{};
    };
}
