# DDA recordings

What a desktop-duplication tonemap stage is handed, recorded for the dirty-rect replay
harness (`Tests/Rendering/DdaReplayTests.cpp`, runner option `--recording <dir>`). The
recorder is ShaderLab-private's `tools/dda_recorder`, a one-off console client of
`IDXGIOutputDuplication`; its README says how to run it and how it decides a recording
is void. Recordings are large and stay outside the repositories. The test-side reader
and writer are `Dda::RecordingReader` / `Dda::RecordingWriter` in
`Tests/Rendering/DdaStageSim.{h,cpp}`; `Stage1_RecordingFormat` writes a synthetic
recording and reads it back, so the format is checked without a real one (decision #243).

## Layout

One folder per recording. `--recording` takes the folder above them and replays every
subfolder that has a `frames.bin`.

| File | What it is |
|---|---|
| `frames.bin` | A header, then one record per acquired frame, in order. |
| `key_<frame>.jxr` | The whole surface after frame `<frame>` (six digits), lossless 64bpp RGBA half JPEG XR. Frame 0 is always one. |
| `summary.txt` | The recorder's own report: rates, `AccumulatedFrames`, hold time, readback, the keyframe self-check, and valid or VOID. |

## `frames.bin`

Little-endian, packed to 4 bytes.

```cpp
struct RecordingHeader            // 52 bytes
{
    char     magic[8];            // "DDAREC1\0"
    uint32_t version;             // 1
    uint32_t width, height;       // the duplicated surface
    uint32_t format;              // DXGI_FORMAT_R16G16B16A16_FLOAT (10)
    uint32_t rotation;            // DXGI_MODE_ROTATION of the output
    uint32_t keyframeInterval;    // presents between keyframes
    uint32_t frameCount;          // records that follow
    uint32_t flags;               // 0x1 rotated pairs, 0x2 full frames
    uint32_t reserved;
    int64_t  qpcFrequency;        // for LastPresentTime
};

struct RecordingFrame             // 40 bytes
{
    uint32_t kind;                // 0 a present, 1 pointer only (LastPresentTime 0): NoFrame
    uint32_t accumulatedFrames;   // DXGI_OUTDUPL_FRAME_INFO
    uint32_t rectsCoalesced;
    uint32_t keyframe;            // 1: key_<frame>.jxr holds the frame, no rect pixels follow
                                  // (always 1 for a present with no metadata)
    int64_t  lastPresentTime;
    float    holdMs;              // AcquireNextFrame returning to ReleaseFrame
    uint32_t moveCount;
    uint32_t dirtyCount;
    uint32_t pixelSets;           // 0 for a keyframe or NoFrame; 1, or 2 for a rotated output
};
```

After each record: `moveCount` `DXGI_OUTDUPL_MOVE_RECT` (24 bytes each), then `dirtyCount`
`RECT` (16 bytes each), as `GetFrameMoveRects` and `GetFrameDirtyRects` returned them.
Then, for a frame that is not a keyframe:

- **Set 0**: for each move destination, then each dirty rect, clipped to the surface and
  skipped when empty, its FP16 pixels after the frame, rows packed (8 bytes a pixel).
- **Set 1** (rotated outputs only): the same rects rotated from the desktop's orientation
  into the surface's texel space, each block led by its clipped `RECT` (an empty one has
  no pixels). Which interpretation is right is what the rotated scenario settles; the
  replay uses set 0.

## Replaying one

Moves first, then rect pixels, as the desktop duplication contract says: each move is
copied, in order, from the image as the previous moves left it, through a temporary copy
(source and destination may overlap). A keyframe replaces the whole image; before it
does, the frame's moves are applied and its dirty rects' pixels are taken from the
keyframe, and any other pixel that differs is a change the rects missed. The harness
counts those as the recorder does, and skips a recording where more than 5% of presents
had `AccumulatedFrames > 1` or a keyframe missed anything. A `NoFrame` record is not a
present: the stage does not realize for it.

A present with no metadata (`TotalMetadataBufferSize` 0 with a new `LastPresentTime`)
may have changed anything, so it is Whole: the recorder stores it as a keyframe with no
rects, and the replay reads a present with no moves and no dirty rects as Whole
(`DesktopFrame::whole`): the stage invalidates the whole input, not nothing. Such a
keyframe has no rects to rebuild from, so neither self-check compares it, and a reader
refuses one that is not a keyframe.
