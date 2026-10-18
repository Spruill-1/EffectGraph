# ShaderLab Built-in Effects

ShaderLab ships with **38 built-in ShaderLab effects**, on top of the **40+ wrapped built-in D2D effects** in `Effects/EffectRegistry.cpp`. The 30 shader effects are written in `BuiltInEffects/BuiltInEffectTable.cpp` and reach a program as generated headers with their bytecode prebuilt ([Built-in effects](../architecture/effectgraph-api.md#built-in-effects)); the 8 without a shader (parameters, Clock, Numeric Expression, Random, Working Space) are in `Effects/ShaderLabEffects.cpp`. The shader effects share a common color math library (BT.709 / BT.2020 / DCI-P3 matrices, PQ / HLG transfer functions, CIE xy conversions, ICtCp). Every effect is versioned with `effectId` and `effectVersion` so saved graphs can be upgraded in place — see [Effect Versioning System](effect-versioning.md).

The **Type** column below uses these abbreviations:

- **PS** = Pixel shader (`CustomPixelShaderEffect`, D2D `ID2D1DrawTransform`)
- **CS-Img** = D3D11 compute shader producing an FP16 image output (`CustomComputeBridgeEffect` wrapper, `hasImageOutput = true`)
- **CS-Data** = D3D11 compute shader producing only analysis fields (no image output, `dataOnly = true`)
- **Host** = Implemented host-side; no HLSL (image sources, parameter nodes)

## Analysis → Heatmaps

False-color overlays on the input image.

| Effect | Type | Description |
|--------|------|-------------|
| Luminance Heatmap | CS-Img | False-color BT.709 luminance overlay (Turbo / Inferno gradients). |
| Luminance Highlight | CS-Img | Highlights luminance bands above/below configurable thresholds. |
| Delta E Comparator | CS-Img | Two-input perceptual difference map (Heatmap or Grayscale dE). `Method` selects CIE76 / CIE94 / CIEDE2000 / **ΔE ITP (BT.2124)** — ITP is the default and the right choice for anything HDR or wide-gamut; the three Lab metrics were fit to reflective samples under SDR viewing and leave their domain above ~100 nits. Measured on a 1100 vs 1000 nit step: ITP 7.47, CIEDE2000 118.09, CIE76 253.26. One unit ≈ 1 JND in all four, so the numbers stay comparable when switching. |
| Gamut Highlight | PS | Highlights pixels outside a target gamut (sRGB / Display P3 / BT.2020 / Custom / DCI-P3; see [Target gamut ids](#target-gamut-ids)). |
| Nit Map | PS | Display-referred nit visualization with configurable luminance bands. |

## Analysis → Scopes

Plot the input image's chromaticity or luminance distribution.

| Effect | Type | Description |
|--------|------|-------------|
| CIE Histogram | CS-Img | 2D histogram of pixel chromaticity on the CIE xy plane (x 0.3127 +- 0.5, y 0.3290 +- 0.5, the window the CIE Chromaticity Plot reads), **one bin per `BinSize` block of output pixels**: `OutputSize` (64-4096, default 512) is the image size and `BinSize` (1x1 to 10x10, default 1x1) the block, so there are ceil(OutputSize / N) bins per side -- 16.7M at 4096 and 1x1. Each bin's value fills its whole block (flat blocks; the last row and column are clipped at the image edge), and counts, the max-bin normalisation and the average luminance are per bin. R = log2(count+1) / log2(maxCount+1), G = average luminance (nits) / 10000, A = 1 where any pixel landed; pixels with alpha < 0.01 or no chromaticity (black) are skipped, and negative scRGB is kept so wide-gamut colours land outside the sRGB triangle. Runs as two dispatches (`SHADERLAB_IMAGE_PASS`, v5): one thread per source pixel scatters into a per-bin accumulator, merging each 16x16 tile's pixels by bin in groupshared first, then one thread per touched bin fills its block. The accumulator is 16 bytes per output pixel whatever the bin size (4 MB at 512, 256 MB at 4096) on top of the FP32 output image. Through v4 the histogram was a fixed 48x48 tile scaled up to the output. |
| CIE Chromaticity Plot | PS | Plots image pixels on a CIE 1931 xy diagram with gamut triangle overlays. |

## Analysis → Statistics (data-only)

CPU-readable reductions over the input image. Bind their analysis fields to downstream properties via the property-binding system.

| Effect | Type | Description |
|--------|------|-------------|
| Channel Statistics | CS-Data | Per-channel R / G / B / A min / max / mean / median / P95 / nonzero%. **`HighlightP99`** (v6) is the 99th percentile of the pixels `ClippedFraction` counts, read from the top of the same histogram, and the clip threshold when none are above white: the top of the highlights whether they fill the frame or one small window, which whole-frame `P99` is not. **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Luminance Statistics | CS-Data | BT.709 Y stats with HDR-aware extras (log-spaced histogram, AvgLog, ClippedFraction). **`ClippedFraction`** is the fraction of pixels whose luminance is more than 1% above `ClipNits` (`Y * 80 > ClipNits * 1.01`); `WhiteErrorBudget = min(ClippedFraction * BudgetScale, BudgetMax)`, with `BudgetScale` default 2 and `BudgetMax` 20: a tone mapper bound to it spends at most 2 dE ITP at SDR white even on a frame that is entirely above white, and a few tenths for an HDR window on a desktop (earlier 1.9.0 builds defaulted to 15; decision #117). The margin keeps SDR white out of the count: a desktop stores it as `W / 80` in FP16, which reads back at or an FP16 step above `W`, and 1% above white is under 0.8 dE ITP from it (v5; through v4 the rule was `>= ClipNits`, which counted every white UI pixel). **`HighlightP99`** (v6) is the 99th percentile of the pixels `ClippedFraction` counts, read from the top of the same histogram, and the clip threshold when none are above white: the top of the highlights whether they fill the frame or one small window, which whole-frame `P99` is not. **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Chromaticity Statistics | CS-Data | ICtCp Ct/Cp stats (mean / max chroma, mean hue). **`HighlightP99`** (v6) is the 99th percentile of the pixels `ClippedFraction` counts, read from the top of the same histogram, and the clip threshold when none are above white: the top of the highlights whether they fill the frame or one small window, which whole-frame `P99` is not. **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Image Info | CS-Data | Image metadata (width, height, format-derived nit assumptions). |

## Analysis → Tone Mapping (ICtCp suite)

HDR ↔ SDR operators built around BT.2100 ICtCp. The key property: I (intensity) is decoupled from Ct/Cp (chromaticity), so compressing or expanding I alone preserves hue and saturation by construction. This is the design thesis of the suite: operations that need to be carefully done in linear RGB or CIE xyY (per-channel hue shifts, gamut excursions, chromaticity drift) reduce to one-line manipulations of I in ICtCp. Each operator exposes its nit-targets as numeric parameters — wire them from `working_space.SdrWhiteNits` / `PeakNits` to track the OS slider or simulated profile automatically.

| Effect | Type | Description |
|--------|------|-------------|
| ICtCp Round-Trip Validator | PS | Diagnostic: outputs `\|scRGB→ICtCp→scRGB - in\| × Gain`. Should render black on a correct image; non-zero output indicates a bug in the ICtCp conversion. |
| ICtCp Tone Map (HDR → SDR) | CS-Img | I-channel Reinhard compression with `ToneLift` polynomial bump. Source / target peaks specified in nits; Ct/Cp pass through unchanged. |
| ICtCp Inverse Tone Map (SDR → HDR) | CS-Img | Mirror of the above; inverse Reinhard expands SDR-anchored content into the HDR peak. |
| ICtCp Saturation | PS | Per-pixel saturation gain in ICtCp (scales Ct/Cp around the I-axis). |
| ICtCp Highlight Desaturation | CS-Img | Smoothly reduces saturation above a configurable I threshold (counteracts tone-map hue shifts at clipping). |
| ICtCp Gamut Boundary LUT | CS-Img | **A generator** (no image input) that bakes `B(I, hue)` — the gamut boundary radius `CubeBoundaryRadius` finds — into a 2048×128 FP32 table for a consumer's lookup input (read with `GamutLutBoundaryRadius`). **v3:** 2048 hue columns (was 256) -- a gamut boundary peaks sharply at each corner hue and linear interpolation between 256 columns cut up to 5.6% off it -- and the stamp's alpha carries `GAMUT_LUT_STAMP_OFFSET` (half the width), so a reader built for another width refuses the table instead of misreading it. It exists because on real wide-gamut HDR content (a PQ/BT.2020 capture) a gamut-compressing tone mapper's boundary searches were **81% of its cost**, and `B` depends only on `(I, hue, peak, target gamut)` — nothing in the image — so it can be built once and reused every frame until the display's SDR white moves. The generator and its readers include the **same** header, `shaderlab_gamut.hlsli` (search, geometry, row warp, stamp and reader in one copy), so a table can only ever be a sampled copy of the exact answer. **Give it the same `SdrWhiteNits` and `TargetGamut` as its consumer** (bind both to the same Working Space node): every texel is stamped with the peak and a fingerprint of the target gamut it was built for, so a consumer can *refuse* a table built for another peak or gamut and fall back to the exact search — slower, never wrong. One table is one target: an sRGB and a P3 consumer each want their own LUT node (a rebuild is ~0.04 ms of GPU, so the graph is the cache). Rows are packed toward I(W) (`GAMUT_LUT_WARP_K` = 2), where the boundary collapses and the knee lands every bright pixel: measured against the exact search, real HDR content (bird capture) mean 0.0034 / max 2.2 ΔE ITP; a saturated BT.2020 gradient -- the worst case, nearly every pixel in that band -- mean 0.18 / max 3.3 (uniform rows: 0.42 / 6.1). Output size is fixed (hidden `OutputWidth/Height`) because the reader indexes it with the same constants. **v4:** `TargetGamut` gains DCI-P3 (id 4); see [Target gamut ids](#target-gamut-ids). |
| Gamut LUT Viewer | CS-Img | Makes a boundary table visible. Inputs `LUT A` and optional `LUT B`, both lookup inputs read with `Load`; each table's stamp supplies its own peak and I range, so the viewer has no `SdrWhiteNits`. **Slice map** (`Mode` 0): hue across (-pi..pi), I up, linear from black at the bottom to the table's I(W) at the top, each cell the boundary colour `ICtCpToScRGB(I, B dir(hue))`; with `LUT B` wired the image splits, A on top, B below. **Polar slice** (`Mode` 1): the Ct/Cp plane at `SliceI` (a fraction of table A's I range), A's in-gamut colours dimmed, A's boundary in white and B's in orange, rings every 0.05, the neutral axis at the centre; `FitMode` sizes the plot to the larger boundary at that I or to a fixed `Extent`. `Normalize` = `Per-pixel` scales each colour so its brightest channel is 1 (signed values kept, so wide-gamut colours still clip in an SDR capture); `ShowPrimaries` marks the hue of each sRGB, Display P3 and BT.2020 primary at the table's SDR white. `OutputWidth` / `OutputHeight` size the image (default 1024×512). A pin wired to something that is not a table shows magenta stripes; an unwired A (with B wired) shows grey stripes. `Mode` is a specialised option. |

## Analysis → Gamut

### Target gamut ids

Every effect with a `TargetGamut` / `SourceGamut` / `Gamut` option, and the `Gamut Parameter` node, shares one numbering. Graphs store the number, so new gamuts are appended.

| Id | Label | Primaries | White |
|----|-------|-----------|-------|
| 0 | sRGB | BT.709 | D65 |
| 1 | Display P3 | P3 | D65 |
| 2 | BT.2020 | BT.2020 | D65 |
| 3 | Custom | `RedPrimary` / `GreenPrimary` / `BluePrimary` (bind to Working Space) | D65, or `WhitePoint` where the effect has one (ICtCp Gamut Boundary LUT) |
| 4 | DCI-P3 | P3 | DCI (0.314, 0.351), Bradford-adapted to D65 |

A non-D65 white is **adapted**, as `MakeTargetXf` does for Custom: DCI white (1, 1, 1) in DCI-P3 is scRGB (1, 1, 1), and scRGB white stays neutral in the target. The effects that work on CIE xy triangles use the adapted primaries (`GAMUT_DCIP3_R/G/B` in `shaderlab_colormath.hlsli`: R (0.680701, 0.318895), G (0.281207, 0.674168), B (0.148832, 0.057667)), so every effect sees the same gamut. Id 1 was labelled "DCI-P3" until v1.9.0 but always computed Display P3.

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Coverage | CS-Img | Percentage of target gamut volume covered by input (image overlay + analysis field). **`HighlightP99`** (v6) is the 99th percentile of the pixels `ClippedFraction` counts, read from the top of the same histogram, and the clip threshold when none are above white: the top of the highlights whether they fill the frame or one small window, which whole-frame `P99` is not. **Multi-group (2026-09-25):** runs as 64 thread groups that add their bins into global counters, the last group to finish rendering the diagram, not one group walking the frame on a single CU. Its two 64×64 bin arrays fill the 32 KB groupshared limit, so it uses `SHADERLAB_REDUCE_SCRATCH_SHARED_FLAG(gs_inGamut[0])`: the last-group flag borrows bin 0 after the bins are folded. |
| ICtCp Boundary | PS | ICtCp gamut boundary visualization for a chosen target gamut. **v5:** the six boundary polygons are built on the CPU once per parameter change (derived constants) instead of per pixel: 6.1 -> 2.8 ms at 1 Mpx. |

## Color Processing

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Map | PS | CIE xy gamut mapping: Clip / Nearest / Compress to White / Fit Gamut. **Clip simulates a display:** convert to the target's RGB and clamp each channel to `[0, ClipNits / 80]`, i.e. every primary saturates at the display's full drive. Bind `ClipNits` to Working Space `PeakNits` (this panel in HDR mode) or `SdrWhiteNits` (an SDR presentation), and the target primaries to Working Space for a Custom target. `ClipNits = 0` clamps only negatives (the pre-v7 behaviour, which passed HDR highlights straight through). v6: Clip honours a Custom target -- through v5 it silently clipped Custom to BT.2020. |
| ICtCp Gamut Map | PS | Perceptual gamut mapping in BT.2100 ICtCp space: Nearest on Shell / Compress to Neutral / Fit to Shell. (Renamed from `Perceptual Gamut Map` in v1.3.8 — old graphs load via legacy alias.) **v13:** the target boundary (64 levels of I) and Fit to Shell's scale (256 levels) are CPU-built tables, interpolated in I, instead of being rebuilt per pixel -- Fit to Shell 1,924 -> 2.0 ms at 1 Mpx. Max 0.11 dE ITP from the exact per-pixel result. |
| Scale | CS-Img | Resample the input to a target width × height with selectable filter (bilinear / Catmull-Rom / Mitchell-Netravali / Lanczos-3 / box / Gaussian). Used to throttle preview resolution for heavy chains without losing analysis-branch fidelity. **v2:** separable -- each tile filters the source rows it needs horizontally, then vertically -- so a Lanczos-3 4x downscale takes 1.9 ms instead of 94.6. Differs from v1 by float rounding (<= 7e-6). |

## Source / Generator

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Source | PS | Swept gamut fill for a target color space. |
| Color Checker | PS | Macbeth ColorChecker pattern with accurate sRGB patches. |
| Zone Plate | PS | Sine-wave zone plate for resolution / aliasing testing. |
| Gradient Generator | PS | Configurable linear / radial gradient with HDR range. |
| HDR Test Pattern | PS | Luminance step wedge from 0 to 10,000 nits. |
| Image Source | Host (WIC) | Static image file. **Whatever WIC can decode on the machine** — the file-picker filter is built from `ImageLoader::SupportedExtensions()`, which enumerates the registered WIC decoders, so HEIF/AVIF/JPEG XL and camera RAW appear when their codecs are installed (65 extensions on a stock Windows 11 box with the HEIF extension). Decoding always produces linear FP16 scRGB. A file that cannot be decoded or colour-managed sets `runtimeError` with the failed stage and HRESULT instead of rendering nothing or rendering encoded pixels; a file saved over is decoded again the next time the node is prepared. |
| Video Source | Host (Media Foundation) | Decodes a video file and shows the frame for its `Time` (usually bound to a Clock), decoding a few frames ahead and dropping frames when ticks are slower than the clip; see [video playback](https://github.com/Spruill-1/ShaderLab/blob/main/docs/ui-ux/animation-system.md#video-playback). Colour comes from the stream’s tags: PQ and HLG HDR, SDR, BT.601/709/2020 matrices and full or limited range ([source runtime](../architecture/effect-graph-model.md#video)). |
| Live source | Host | Frames the host produces, through a live source kind it registers ([live sources](../architecture/effect-graph-model.md#live-sources)). EffectGraph has none built in; ShaderLab registers DXGI Desktop Duplication and Windows Graphics Capture. |

## Composition

| Effect | Type | Description |
|--------|------|-------------|
| Split Comparison | PS | **v9: up to eight inputs in equal wedges around the image centre**, rotated by `Rotation` (the only parameter). The node shows the pins in use plus one spare (variadic inputs): connect a third input and a fourth pin appears. Two inputs at rotation 0 split along a vertical line, first input on the left, as v8's default did. A disconnected middle pin leaves no empty wedge. v8's `SplitPosition`, `LineWidth` and `Angle` are gone. For whole images next to each other, see Side by Side. |
| Side by Side | CS-Img | **Every connected input, whole, in one image** -- where Split Comparison shows one wedge of each. Up to eight variadic inputs, as Split Comparison; a disconnected middle pin leaves no empty tile. `Layout`: `Row` (default), `Column`, or `Grid` (columns = ceil(sqrt(n)), filled row by row; spare cells are background). Each tile is the size of the first connected input; an input of another size is centred in its tile, scaled down to fit (aspect kept) if it is larger, never cropped. `SizeMode` `Native` (default) makes the output the arrangement at 1:1 -- `cols × W + gaps` by `rows × H + gaps` -- and reads each input with `Load`, so values pass through bit-exact, signed scRGB included; `Fit` scales the whole arrangement uniformly into `OutputWidth × OutputHeight` (default 1920×1080), centred and letterboxed, with bilinear filtering of linear values (up to 4×4 bilinear taps over the footprint when reducing). `Gap` (px, default 0) separates tiles; `Background` (float4 scRGB, default opaque black) fills gaps, empty cells and letterboxing. A D3D11 compute effect because a pixel shader sees only the input region under each output pixel and each tile reads its input at an offset; the output size comes from the `deriveImageOutputSize` hook (decision #118). An output past 16384 px on a side is refused with a node error: three 3840-wide inputs in a row (11520×2160) fit, five do not -- use `Grid` or `Fit`. Cost: the inputs are copied to FP32 textures (133 MB each at 4K) and the output is FP32 (398 MB at 11520×2160); the dispatch itself measured 5.0 ms for 11520×2160 on Adreno, and a 3840×2160 input reduced into a 1024×1024 tile 4.6 ms (v2; 25.5 ms in v1, whose filter selected the input texture at every tap). |

## Data / Parameter Nodes

| Node | Description |
|------|-------------|
| Float Parameter | Continuous slider; teal node with inline canvas slider. |
| Integer Parameter | Discrete slider. |
| Toggle Parameter | Boolean on / off. |
| Gamut Parameter | Gamut-id selector (sRGB / Display P3 / BT.2020 / Custom / DCI-P3; see [Target gamut ids](#target-gamut-ids)). |
| Clock | Time source: outputs `Time` (seconds) and `Progress` (0–1 over a configurable duration). Drives the animation system. |
| Numeric Expression | Single configurable math node powered by ExprTk; user-supplied formula evaluated against dynamic float inputs `A..Z`. Replaces the older Add / Subtract / Multiply / Divide / Min / Max nodes. See [Numeric Expression Node](numeric-expression.md). |
| Random | Takes a single `Seed` float input and outputs a deterministic, well-mixed `Result` in `[0, 1)`. The output is a pure function of the seed (SplitMix64-style integer mixer on the float's bit pattern), so identical seeds always reproduce identical values and any change — e.g. a tick from an upstream Clock or Numeric Expression — yields a fresh random number. |
| Working Space | Strict sink (no input pins, no output image pin) that mirrors the active display profile from the top-bar profile selector — live OS-reported caps or whatever simulated preset / ICC the user has applied. Exposes 14 typed analysis output fields: `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus the four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2). Bind any downstream property to these fields via the property-binding system to drive an effect from the live working space — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it will track Display Settings or simulated profile changes automatically without touching the graph. |


---

Back to [docs/](../README.md) • [Repo root](../../README.md)
