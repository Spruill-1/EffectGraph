#pragma once

// A stand-in for the desktop-duplication tonemap stage (test code only): the
// HDR desktop it is handed, its per-present realize into an _SRGB
// intermediate, one accumulator per client, the copy into each client's UNORM
// surface, the retry tick, and the recordings it can be replayed from.
// docs/architecture/effectgraph-api.md (Dirty rects) states what a real stage
// owes; this simulates it so the replay cases can check the engine against it.

#include "pch_engine.h"
#include "Engine/Api/EffectGraph.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ShaderLab::Tests::Dda
{
    // ---- Frames -----------------------------------------------------------

    enum class FrameKind : uint32_t
    {
        Present = 0,
        // A pointer-only frame (LastPresentTime 0): not a present.
        NoFrame = 1,
    };

    // What one duplication frame reported, as DXGI_OUTDUPL_FRAME_INFO and the
    // metadata calls give it.
    struct DesktopFrame
    {
        FrameKind kind{ FrameKind::Present };
        int64_t lastPresentTime{ 0 };
        uint32_t accumulatedFrames{ 1 };
        bool rectsCoalesced{ false };
        float holdMs{ 0.0f };
        std::vector<EFFECTGRAPH_MOVE_RECT> moves;
        std::vector<RECT> dirty;
        // A present with no metadata (TotalMetadataBufferSize 0 with a new
        // LastPresentTime): anything may have changed, so it is Whole.
        bool whole{ false };
    };

    // Everything the frame changed: move destinations, then dirty rects.
    std::vector<RECT> ChangedRects(const DesktopFrame& frame);

    // ---- The HDR desktop ----------------------------------------------------

    // The desktop surface: R16G16B16A16_FLOAT scRGB, mirrored on the CPU.
    class Desktop
    {
    public:
        Desktop(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        ID3D11Texture2D* Texture() const { return m_texture.get(); }
        std::vector<uint16_t>& Texels() { return m_texels; }
        const std::vector<uint16_t>& Texels() const { return m_texels; }

        void Put(uint32_t x, uint32_t y, float r, float g, float b);
        // Every move, in order, each from the image as it was before the
        // frame (DDA order: moves before dirty rects).
        void ApplyMoves(const std::vector<EFFECTGRAPH_MOVE_RECT>& moves);
        // FP16 pixels for `rect`, rows packed, into the CPU image.
        void WriteRect(const RECT& rect, const uint16_t* pixels);
        std::vector<uint16_t> ReadRect(const RECT& rect) const;
        // The CPU image into the texture, one update per rect.
        void Upload(const std::vector<RECT>& rects);
        void UploadAll();

    private:
        uint32_t m_width, m_height;
        std::vector<uint16_t> m_texels;
        winrt::com_ptr<ID3D11Texture2D> m_texture;
        ID3D11DeviceContext* m_context;
    };

    // A scripted desktop: each call changes the CPU image the way a desktop
    // would and reports it. Returns false when the scenario is over.
    class Scenario
    {
    public:
        virtual ~Scenario() = default;
        virtual const char* Name() const = 0;
        // Paints frame 0 (the desktop before the first present).
        virtual void Start(Desktop& desktop) = 0;
        virtual bool Next(Desktop& desktop, DesktopFrame& frame) = 0;
    };

    enum class ScenarioKind
    {
        IdleCursor,      // a blinking caret, pointer-only frames between; every
                         // fifth present reports no metadata (Whole)
        Scroll,          // an SDR page scrolling: a move plus the strip it exposes
        HdrVideo,        // an HDR video window, new content every present
        WindowDrag,      // a window dragged: a move plus the exposed background
        CoverAll,        // rects covering the whole desktop every present
        VideoAndScroll,  // a video beside a scrolling page, > 16 rects a frame
        HdrHighlights,   // an HDR highlight climbing and a block over SdrWhiteNits
                         // toggling (stage 3: commits to land)
    };
    std::unique_ptr<Scenario> MakeScenario(ScenarioKind kind, uint32_t frames);
    const char* ScenarioName(ScenarioKind kind);

    // ---- The stage ----------------------------------------------------------

    // What an acquire handed the client.
    struct Delivery
    {
        HRESULT hr{ S_OK };            // S_OK or DXGI_ERROR_WAIT_TIMEOUT
        bool whole{ false };
        std::vector<RECT> rects;       // dirty rects (moves delivered as dirty)
        uint32_t presents{ 0 };        // successful realizes accumulated
        // With moves (SetMoves) and one contributing realize: what it
        // reported in `moved`, in order (their destinations are in `rects`
        // too). Empty when several realizes contributed: their moves are
        // delivered as dirty rects only.
        std::vector<EFFECTGRAPH_MOVE_RECT> moves;
        // The client contract's dirty rects: applied after `moves` to the
        // client's stored copy, they make it the surface. `rects` without the
        // move destinations when `moves` is set; otherwise `rects`.
        std::vector<RECT> dirty;
    };

    // The stage's Hosted policy driver (section 0.5): decides from the landed
    // value (TryGetValues) and commits (CommitFields) before the realize. The
    // base commits nothing and never ratchets: a graph with no Hosted field.
    class HostedDriver
    {
    public:
        virtual ~HostedDriver() = default;
        // The damping decision; true when it committed. `present` is false for
        // a retry tick, which does not count toward a decay.
        virtual bool OnPresent(bool present) { (void)present; return false; }
        // The landed value would ratchet the committed one up: a tick is due.
        virtual bool WouldRatchet() const { return false; }
    };

    // The provisional damping (design section 8; harness values until stage 2
    // data sets them): ratchet up at once; after `decayPresents` presents in a
    // row below the committed value, step down to the landed value. Ticks do
    // not count.
    class DampedHostedDriver : public HostedDriver
    {
    public:
        DampedHostedDriver(IEffectGraphNode* producer, const wchar_t* field, uint32_t decayPresents = 30);
        bool OnPresent(bool present) override;
        bool WouldRatchet() const override;
        uint32_t Ratchets() const { return m_ratchets; }
        uint32_t Decays() const { return m_decays; }

    private:
        // The field's landed and committed values; false when either has none.
        bool Values(float& landed, float& committed) const;

        winrt::com_ptr<IEffectGraphNode2> m_producer;
        std::wstring m_field;
        uint32_t m_decayPresents;
        uint32_t m_below{ 0 };
        uint32_t m_ratchets{ 0 }, m_decays{ 0 };
    };

    class StageSim
    {
    public:
        // `input` is the desktop's texture input, `root` the node realized
        // into `intermediate` (an _SRGB texture on the context's device).
        StageSim(IEffectGraphContext* context, IEffectGraphNode* input, IEffectGraphNode* root,
                 ID3D11Texture2D* intermediate, ID3D11DeviceContext* immediate);

        // A client surface (B8G8R8A8_UNORM, the context's device). With a
        // keyed mutex, the copy into it takes key 0 and hands over key 1.
        uint32_t AddClient(ID3D11Texture2D* surface, IDXGIKeyedMutex* mutex = nullptr);

        // The first realize, before any present: whole.
        HRESULT Prime();
        // One present: its moves (as moves with SetMoves, else as dirty rects
        // at their destinations), its dirty rects, then one realize. A present
        // with no metadata invalidates the whole input.
        HRESULT Present(const DesktopFrame& frame);
        // Stage 5: hand the engine the frame's moves (MoveRects) and forward
        // what each realize reports in `moved`: the client copy takes their
        // destinations as well as `drawn`.
        void SetMoves(bool moves) { m_moves = moves; }
        // The retry tick: due while the last realize failed or left a readback
        // to collect (S_FALSE), or a Hosted value would ratchet; the present's
        // sequence with no new rects.
        bool TickDue() const;
        HRESULT Tick();

        // Stage 3. The Hosted driver (the base commits nothing).
        void SetHosted(std::unique_ptr<HostedDriver> hosted) { m_hosted = std::move(hosted); }
        // Poll first (the stage): PollReadbacks, the damping decision, then one
        // realize. Off: the drain arm, a generic event-driven COM host that
        // realizes first, decides after, and realizes once more when it committed.
        void SetPollFirst(bool pollFirst) { m_pollFirst = pollFirst; }
        // The last present or tick: realizes made, those that drew the whole
        // target, and whether the Hosted driver committed.
        uint32_t Realizes() const { return m_realizes; }
        uint32_t WholeDraws() const { return m_wholeDraws; }
        bool HostedCommitted() const { return m_hostedCommitted; }

        // The client's next frame: DXGI_ERROR_WAIT_TIMEOUT (accumulator kept)
        // while the last realize failed; otherwise the accumulated union is
        // copied from the intermediate into its surface and handed over.
        Delivery Acquire(uint32_t client);

        HRESULT LastRealize() const { return m_lastRealize; }
        const std::vector<RECT>& LastDrawn() const { return m_lastDrawn; }
        const std::vector<EFFECTGRAPH_MOVE_RECT>& LastMoved() const { return m_lastMoved; }
        // Pixels of the intermediate (row-major, one byte each) moved since a
        // realize last drew them: where a dithering shift-invariant effect's
        // copied output may differ from a fresh render. A whole draw clears it.
        // Kept only after SetTrackDrift(true); empty otherwise.
        const std::vector<uint8_t>& Drift() const { return m_drift; }
        void SetTrackDrift(bool on) { m_trackDrift = on; }
        HostedDriver& Hosted() { return *m_hosted; }

    private:
        struct Client
        {
            winrt::com_ptr<ID3D11Texture2D> surface;
            winrt::com_ptr<IDXGIKeyedMutex> mutex;
            std::vector<RECT> rects;
            std::vector<EFFECTGRAPH_MOVE_RECT> moves;
            std::vector<RECT> drawn;  // the realizes' `drawn` alone
            bool whole{ true };       // a new client gets the whole surface
            uint32_t presents{ 0 };
            uint32_t contributing{ 0 };   // realizes that drew or moved something
        };

        HRESULT Realize();
        // The stage 3 sequence after the rects are reported.
        HRESULT Decide(bool present);

        winrt::com_ptr<IEffectGraphContext2> m_context;
        bool m_pollFirst{ true };
        uint32_t m_realizes{ 0 }, m_wholeDraws{ 0 };
        bool m_hostedCommitted{ false };
        winrt::com_ptr<IEffectGraphInput2> m_input;
        winrt::com_ptr<IEffectGraphNode> m_root;
        winrt::com_ptr<ID3D11Texture2D> m_intermediate;
        ID3D11DeviceContext* m_immediate;
        std::vector<Client> m_clients;
        std::unique_ptr<HostedDriver> m_hosted;
        HRESULT m_lastRealize{ S_OK };
        std::vector<RECT> m_lastDrawn;
        std::vector<EFFECTGRAPH_MOVE_RECT> m_lastMoved;
        std::vector<uint8_t> m_drift;
        bool m_anyDrift{ false };
        bool m_trackDrift{ false };
        bool m_moves{ false };
        uint32_t m_width{ 0 }, m_height{ 0 };
    };

    // ---- Recordings -----------------------------------------------------------
    //
    // The PRIV recorder's format (docs/development/dda-recording.md): one
    // folder per recording holding frames.bin and key_<frame>.jxr keyframes.

#pragma pack(push, 4)
    struct RecordingHeader
    {
        char magic[8];              // "DDAREC1\0"
        uint32_t version;           // 1
        uint32_t width, height;     // the duplicated surface
        uint32_t format;            // DXGI_FORMAT of the surface
        uint32_t rotation;          // DXGI_MODE_ROTATION
        uint32_t keyframeInterval;
        uint32_t frameCount;
        uint32_t flags;             // cRecordingRotatedPairs, cRecordingFullFrames
        uint32_t reserved;
        int64_t qpcFrequency;
    };

    struct RecordingFrame
    {
        uint32_t kind;              // FrameKind
        uint32_t accumulatedFrames;
        uint32_t rectsCoalesced;
        uint32_t keyframe;          // 1: key_<index>.jxr holds the frame. A present
                                    // with no moves and no dirty rects had no
                                    // metadata: it is Whole, and always a keyframe
        int64_t lastPresentTime;
        float holdMs;               // AcquireNextFrame to ReleaseFrame
        uint32_t moveCount;
        uint32_t dirtyCount;
        uint32_t pixelSets;         // 0 for a keyframe or NoFrame, else 1 or 2
        // Then moveCount DXGI_OUTDUPL_MOVE_RECT and dirtyCount RECT. Set 0:
        // the FP16 pixels of each move destination then each dirty rect,
        // clipped to the surface, rows packed. Set 1 (rotated outputs): the
        // same rects rotated into texel space, each block led by its RECT.
    };
#pragma pack(pop)
    static_assert(sizeof(RecordingHeader) == 52 && sizeof(RecordingFrame) == 40, "the recorder's layout");

    inline constexpr uint32_t cRecordingVersion = 1;
    inline constexpr uint32_t cRecordingRotatedPairs = 0x1;   // set 1: rects rotated into texel space
    inline constexpr uint32_t cRecordingFullFrames = 0x2;     // every frame a keyframe

    // Writes and reads one recording; the replay cases use the reader, and
    // a synthetic recording written in process checks the format round trip.
    class RecordingWriter
    {
    public:
        RecordingWriter(const std::filesystem::path& folder, uint32_t width, uint32_t height,
                        uint32_t keyframeInterval);
        ~RecordingWriter();
        // The frame's metadata, and its pixels from the desktop after it.
        bool Write(const DesktopFrame& frame, const Desktop& after);
        bool Close();

    private:
        std::filesystem::path m_folder;
        FILE* m_file{ nullptr };
        RecordingHeader m_header{};
        uint32_t m_index{ 0 };
    };

    class RecordingReader
    {
    public:
        explicit RecordingReader(const std::filesystem::path& folder);
        ~RecordingReader();
        bool Ok() const { return m_ok; }
        const std::string& Error() const { return m_error; }
        const RecordingHeader& Header() const { return m_header; }
        // The next frame into `frame`, applied to `desktop` (moves, then
        // pixels or the keyframe). `keyframeMismatches` counts the pixels a
        // keyframe found that the rects missed (the recorder's self-check).
        bool Next(Desktop& desktop, DesktopFrame& frame, size_t& keyframeMismatches);
        uint32_t Index() const { return m_index; }

    private:
        std::filesystem::path m_folder;
        FILE* m_file{ nullptr };
        RecordingHeader m_header{};
        uint32_t m_index{ 0 };
        bool m_ok{ false };
        std::string m_error;
    };

    // Lossless 64bpp RGBA half JPEG XR, through WIC.
    bool WriteHalfJxr(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint16_t* texels);
    bool ReadHalfJxr(const std::filesystem::path& file, uint32_t width, uint32_t height, std::vector<uint16_t>& texels);

    // ---- Helpers ------------------------------------------------------------

    uint16_t ToHalf(float value);
    float FromHalf(uint16_t half);
    bool Inside(const std::vector<RECT>& rects, uint32_t x, uint32_t y);
    // A 4-byte-per-pixel texture's bytes, rows packed (on its own device).
    std::vector<uint8_t> ReadBytes(ID3D11Texture2D* texture);
    // Pixels differing between two images, and those outside `rects`.
    struct PixelDiff { size_t differing{ 0 }; size_t outside{ 0 }; };
    PixelDiff Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t width,
                      const std::vector<RECT>& rects);
}
