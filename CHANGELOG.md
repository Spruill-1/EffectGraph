# Changelog

All notable changes to EffectGraph are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions follow
`Engine/Api/EffectGraphVersion.h`. EffectGraph was developed inside
[ShaderLab](https://github.com/Spruill-1/ShaderLab) until 1.0.0; that history is in
ShaderLab's changelog and in [the decision log](docs/history/decision-log.md).

## [Unreleased]

Version 1.1.0 (`EffectGraphVersion.h`), not yet published: dirty rects, stages 0 to 6
of the design, and the fixes and tests that came with them.

### Added

- **Generated headers name each effect's parameters and analysis fields** (decision #270;
  [EffectGraph API](docs/architecture/effectgraph-api.md#built-in-effects)). Beside
  `CLSID_EffectGraph<Name>`, every header `EffectGraphCompile` writes (the built-ins and
  `--header` alike) defines `inline constexpr wchar_t EffectGraph<Name>_Parameters_<Parameter>[]`
  and `EffectGraph<Name>_Fields_<Field>[]` for `Bind`, so a misspelt binding is a compile
  error. A member keeps its case, and each run of other characters becomes one `_`
  (`Nonzero%` -> `EffectGraphChannelStatistics_Fields_Nonzero`). A name with no letters or
  digits, or one colliding with an earlier name in its group, gets a comment instead. The
  new generated `EffectGraphCoreNodes.h` (included by `EffectGraphBuiltIns.h`) gives the
  eight nodes with no shader the same identifiers, and `CreateEffectById` now creates them
  too: `CLSID_EffectGraphWorkingSpace`, `EffectGraphWorkingSpace_Fields_SdrWhiteNits`.
  Tests: `TestEffectIdentifiers` (naming, collisions, escaping), `Blob_HeaderNamesParameters`,
  and `Clsid_CoreNodeCreates` / `Names_*` in `TestBuiltInEffectIds`, which bind a chain with
  the constants alone.
- **Dirty rects** (decisions #238-#243; [EffectGraph API](docs/architecture/effectgraph-api.md#dirty-rects-11)).
  Three new interfaces, reached by `QueryInterface` from the 1.0 ones, whose IIDs and
  vtables do not change: `IEffectGraphInput2::InvalidateRects` (and `MoveRects`),
  `IEffectGraphNode2::GetInvalidRects` (and `TryGetValues`, `CommitFields`,
  `GetCommittedValues`), and `IEffectGraphContext2::RealizeToIncremental` (and
  `PollReadbacks`), plus `EFFECTGRAPH_REALIZE_FLAGS`, `EFFECTGRAPH_MOVE_RECT` (laid out as
  `DXGI_OUTDUPL_MOVE_RECT`, checked by a `static_assert`), the context flags
  `EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK` / `_DEFERRED_ANALYSIS_COMMIT` and
  `EFFECTGRAPH_E_FALLBACK_BUILD`. Implemented now: `InvalidateRects`, `GetInvalidRects`
  and `RealizeToIncremental`, which draws only the rects that changed since the node's
  last incremental realize into a target the host keeps, clipped, with no clear. The
  deferred-commit methods and flag are implemented too (below), and so are `NO_FALLBACK`
  and `MoveRects` with `moved` (below). The C# bindings declare the three interfaces
  (`AsNode2`, `AsInput2`, `AsContext2`), `Rect`, `MoveRect`, `RealizeFlags`, the flags and
  `HResults.FallbackBuild`; the smoke test checks the casts and the not-yet methods on
  x64, ARM64 and portable, and `packaging/Pack.ps1` runs it again with `--expect-v1`
  over the published 1.0.2 `EffectGraph.dll` (`-V1Version`, x64 and ARM64), where the
  context, an effect and an input each answer all three v2 IIDs with `E_NOINTERFACE`
  and the 1.0 calls still work.
- **Dirty regions in the engine.** `EffectNode`'s dirty flag is private, with a
  `Graph::DirtyRegion` beside it (None, up to 16 rects, Whole) behind `MarkDirty()`,
  `MarkDirty(region)`, `ClearDirty()` and `IsDirty()`; every former `dirty = true` is
  Whole. Regions travel through `Graph::MapRegion` (rects only into a ShaderLab pixel
  shader's image pins and Output nodes), `GraphModel::PropagateDirtyDownstream`,
  `PassState::outputChanged` (now id -> region) and `NodeRuntime::pendingRegion`, which a
  host clears through `FrameRequest::acknowledgeRoots`. Live sources report rects with
  `ILiveSource::DirtyRects` (default: none, i.e. Whole); rects that clip to nothing are
  `NoFrame`. `EvaluatorOptions::dirtyRegions` (on) is the kill switch;
  `verifyDirtyRegions` (off) checks every incremental draw against the pre-draw target
  and a whole render; `GraphEvaluator::RegionStats()` counts draws and pixels.
  `Graph::MarkNodeChanged` takes a region.
- **The analysis change gate** (`EvaluatorOptions::analysisChangeGate`, on; decisions
  #247, #248; [compute](docs/architecture/d2d-d3d11-hybrid-compute.md#known-limitations)).
  A compute node keeps the rects of its image inputs (`Graph::MapRegion`; its output
  stays Whole, `Graph::EmittedRegion`), captured at queue time into
  `NodeRuntime::undispatchedRegion` and readable after a successful dispatch as
  `GraphEvaluator::DispatchInputRegion`. A dispatch that fails (any cause), or a queue dropped
  for a Source with no output, leaves it Whole; a re-dirty between the queue and the
  dispatch (the frozen refresh of an upstream D2D node) is folded into what the dispatch
  consumed. `GraphEvaluator::FailNextComputeDispatchForTest` fails that evaluator's next dispatch for
  tests. An analysis-only compute node no longer dirties
  its bound consumers at its visit; after the dispatch, `PublishFieldsIfChanged` dirties
  only the consumers reading a field whose bits changed, when the dispatch was over rects
  and its own values reached the CPU in that frame, and every consumer otherwise. Verdicts
  are counted in `GraphEvaluator::BindingGate()` (`whole`, `unchanged`, `changed`,
  `noValue`). Off restores the earlier behaviour.
- **`EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK`** (decision #249): a frame that would run a
  fallback build (the build the GPU routing of a binding needs is missing and runtime
  compiles are off) draws nothing, acknowledges nothing and fails with
  `EFFECTGRAPH_E_FALLBACK_BUILD`, or `EFFECTGRAPH_E_COMPILE_PENDING` while that build
  compiles; `FrameRequest::refuseFallbackBuilds` and `FrameResult::fallbackBuilds` /
  `fallbackBuildsPending` / `fallbacksInUse` / `fallbackRefused` carry it. The refusal follows
  the build each needed node is running (`NodeVariantState::fallbackInUse`,
  `GraphEvaluator::FallbacksInUse`), not only the frame that chose it, so a retry with
  nothing new is refused again rather than drawn with the fallback. Pending versus missing is
  judged from the selection that chose the fallback (`VariantChoice::routingPending`,
  `NodeVariantState::fallbackPending`), not from `wanted`, which can still hold an earlier
  selection's compiling key. `GraphEvaluator::ReadbackStats()`
  counts readbacks by cause (`fallbackReadbacks`, `freshReadbacks`,
  `unskippedReadbacks`, `hintReadbacks`, `skippedReadbacks`) and fallback builds, per
  evaluator.

- **Deferred analysis commit** (decisions #250-#255;
  [EffectGraph API](docs/architecture/effectgraph-api.md#deferred-analysis-commit)).
  `EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT` is accepted
  (`EvaluatorOptions::deferredAnalysisCommit`, off by default): no analysis readback is
  ever waited on, and each bound field commits by its binding's policy, the new
  `EFFECTGRAPH_COMMIT_POLICY` (`EXACT` default, `IMMEDIATE`, `THRESHOLD` against the
  field's new `changeThreshold`, `HOSTED`), set with `IEffectGraphNode2::SetCommitPolicy`
  (appended, decision #252), `Graph::SetBindingCommitPolicy`, or `"commitPolicy"` in a
  saved binding. `TryGetValues` (landed), `GetCommittedValues`, `CommitFields` (hosted
  fields, applied at the next frame) and `IEffectGraphContext2::PollReadbacks` are
  implemented and return `E_NOT_VALID_STATE` without the flag; `RealizeToIncremental`
  returns `S_FALSE` while readbacks are in flight. The lane-3 texture is kept texel-equal
  to the committed values by a masked copy (`D3D11ComputeRunner::SetTextureCopy`) and
  direct texel writes on commit and on `SetFloats` / `WriteValues`, which write only the
  named field's texels; a whole-input dispatch stamps the fields so an older readback
  cannot commit after it, and a copy pass that cannot run is not stamped (its landing
  writes the texels; `copyFailures`). A host commit before the seed survives it and the
  field is then judged under its policy; a texture first filled after the seed keeps the
  committed `HOSTED` values; `SetCommitPolicy` re-dispatches the producers so a new policy
  applies at once (decision #254). A host write (`SetFloats` / `WriteValues`) is ordered
  against readbacks in flight: a landing of a dispatch no newer than the write, the seed
  included, leaves the written field alone (`hostWritesKept`); a readback older than a
  whole dispatch never seeds; a readback whose `Map` fails for good reports it on the
  producer and queues a whole dispatch that judges the values again (`readbackFailures`);
  and two bindings that disagree on a field's policy fail every consumer of the field,
  built-in, compute and parameter nodes included, with `Commit policy conflict: ...`
  (decision #255). Luminance Statistics'
  `WhiteErrorBudget` declares `changeThreshold` 0.05 (provisional, decision #251).
  Counters: `GraphEvaluator::DeferredCommit()` and `ReadbackStats().deferredReadbacks`.
  Measured on the Adreno X2-90 (3840x2160, 24 presents, the plugin tone map,
  `Stage3_ReplaySynthetic`): presents that forward the whole target fall to the ones the
  OS itself reports whole (IdleCursor 16.7 per 100 = OS 16.7; Scroll, HdrVideo,
  WindowDrag, VideoAndScroll 0). Mean GPU time per present, retry ticks included, against
  `dirtyRegions` off: 1.810 / 7.229 ms (IdleCursor), 2.557 / 6.141 (Scroll), 11.539 /
  13.220 (HdrVideo), 1.217 / 6.638 (WindowDrag), 2.452 / 4.841 (HdrHighlights); not
  below on VideoAndScroll (11.528 / 11.393) or CoverAll, which is whole on every present
  either way (42.566 / 39.041). On the HdrHighlights scenario a damped hosted peak and the
  threshold budget forward 41.7 whole presents per 100 (5 hosted, 5 threshold, 0
  own-state), commits land 1 frame after their dispatch (mean 1.00, max 1), and polling
  before deciding draws 0.417 whole per present against 0.458 for a stage that decides
  after realizing. Every acquired client surface equals a whole render of the committed
  values. Rects-present CPU time (Debug build) is below stage 2's skip-on stage in every
  scenario (median 0.710-1.181 ms against 1.159-1.543) once the two are timed in
  alternating order after an untimed arm: the first arm to read a frame's desktop upload
  pays for it, which had made stage 3 look up to 0.5 ms dearer on the video scenarios.
  The real-recording exit gate is pending recordings and a margin. Kill switch: omit the
  flag.

- **Cell reductions** (`EvaluatorOptions::incrementalReductions`, on; decisions
  #256-#258; [compute pipeline](docs/architecture/compute-analysis-pipeline.md#statistics-reductions)).
  Channel, Luminance and Chromaticity Statistics reduce over an 8 x 8 grid of cells
  (`SHADERLAB_REDUCE_CELLS` in `shaderlab_params.hlsli`, `DispatchShape::ReduceCells`),
  each cell's partial kept in the scratch buffer between dispatches, so a dispatch whose
  input changed only in rects recomputes just the cells they touch and folds the rest from
  what they kept. Every field is bit-identical to a dispatch over all 64 cells. The
  evaluator passes input 0's rects to the bridge (`ICustomComputeBridge::SetInputDirtyRects`,
  internal); the runner (`D3D11ComputeRunner::SetCellRects`, `CellMaskForRects`) builds
  the cell mask and recomputes every cell unless its last dispatch had the same shader,
  input, scratch, size, format and constants, and that dispatch reached the GPU.
  Counters: `GraphEvaluator::ReduceCells()` (dispatches, those over fewer than 64 cells,
  `ReduceCellsDispatched` / `ReduceCellsTotal`), and per node `ReduceCells(nodeId)`, which
  also gives its last dispatch's cell mask and whether the evaluator handed it rects. Measured on the Adreno X2-90 (Release, 3840x2160 FP16, GPU time of
  a Luminance Statistics frame, median of 21 from an idle GPU): 1 cell 0.474 ms, 16 cells
  0.484, 32 cells 0.743, all 64 0.974, against 1.444 ms for the version 6 interleaved
  reduction; the fixed parts (one cell of an 8 x 8 image) 0.133 ms. On the synthetic
  replays the stage recomputes 4.7-37.5% of the cells (100% on CoverAll and on
  VideoAndScroll, whose 22 rects collapse to a box over every cell). No discrete GPU was
  measured. Kill switch: `incrementalReductions` off recomputes all 64.
- **Move rects** (stage 5, decisions #260-#263; [EffectGraph API](docs/architecture/effectgraph-api.md#dirty-rects-11)).
  `IEffectGraphInput2::MoveRects` is implemented (it returned `E_NOTIMPL`), and
  `RealizeToIncremental` fills `moved`. A move travels as a move (`Graph::DirtyMove` in
  `DirtyRegion`, ordered by a graph-wide sequence, at most 16) into an Output node and into
  a pixel shader whose `CustomEffectDefinition::shiftInvariant` is set (new, saved as
  `"shiftInvariant": true`, carried by `ShaderLabEffectDescriptor` and the effect library)
  through its one image pin; anywhere else, and whenever it cannot be copied, it becomes a
  dirty rect at its destination. An incremental realize copies the root's moves within the
  target (two `CopySubresourceRegion` each through a persistent scratch texture, after the
  session's queued drawing is flushed) and then draws only the rects. A move is drawn
  instead when its source overlaps an older pending rect, when it is from an earlier frame
  (a failed or pending realize: a carry-over), when it reads a destination drawn for either
  reason, when a second `MoveRects` on the input preceded the realize (the first batch),
  on a whole draw, when the moves do not all fit `moved`, and with `applyMoves` off
  (`EvaluatorOptions::applyMoves`, on by default: the kill switch). `moved` reports every
  move handed to an input the root reads since its last successful realize, copied or not.
  `verifyDirtyRegions` replays the copies, and counts pixels moved since they were last
  drawn that differ from a whole render by at most `cMoveDitherCodes` (3) as
  `verifyMoveDrift`; `RegionDrawStats` gains `movesApplied`, `movesCollapsed`,
  `movesReported` and `movePixelsCopied`. ShaderLab-private's tone map (version 25)
  declares `shiftInvariant`, with its position dither as the documented caveat: copied
  pixels differ from a fresh render by at most 2 codes (measured over the scenarios, WARP
  and the Adreno X2-90). Measured on the Adreno X2-90 at 3840 x 2160 with that tone map,
  per present from an idle GPU to idle (wall clock, submission included; the GPU
  timestamps do not bracket copies on this adapter, implying 6.6-22.5 TB/s): scroll
  1.767 ms with the moves copied against 6.124 ms drawing their destinations, window drag
  1.304 against 2.540; the two copies of one scroll destination (3.08 Mpx) alone 0.539 ms
  (91 GB/s). So `applyMoves` ships on. On the synthetic scenarios every move is copied
  (100 per 100 presents in scroll, window drag and video-and-scroll, none collapsed);
  real recordings are pending.
- **Move repairs after review** (decision #264). Frames folded into one realize: a
  `MoveRects` after rects or moves of the same generation starts a new one, so a later
  frame's move whose source an earlier frame's rect changed is drawn, not copied from
  stale pixels (before, it was copied and the destination stayed wrong). Carry-overs are
  judged per root, so with two incrementally realized roots the second's moves are copied
  too (before, every one was drawn). A move whose source leaves the input is a dirty rect
  at its whole destination and is not reported (before, the part whose source was off the
  texture was dropped). `moved` holds only moves inside the target (before, moves outside a
  smaller target were reported). A root left unrealized for 1024 generations with moves
  pending draws whole once and reports none, so the move list stays bounded. A
  `MoveRects` that clips to nothing changes nothing, as documented.

- **Rects through per-pixel Direct2D built-ins** (stage 6, decision #266). `Graph::MapRegion`
  keeps rects (moves collapsed) through every pin of 17 built-ins: Color Matrix, Brightness,
  Contrast, Exposure, Grayscale, Hue Rotation, Invert, Saturation, Sepia, Temperature & Tint,
  Posterize, HDR Tone Map, White Level Adjustment, Opacity, Blend, Composite and Alpha Mask
  (`Graph::RegionIdentityBuiltIns`, `IsRegionIdentityBuiltIn`). Every other built-in is still
  Whole. Each row is admitted only by `TestD2DTableOracle` below; deleting a row withdraws it,
  and `dirtyRegions` off is the switch for all of it.
- **Compute input pre-renders draw only what changed** (stage 6, decision #267). A drawn
  (not in-place) compute input keeps a pending region per target, fed by every output change
  of its producer whether or not a consumer was needed, and redraws only those rects,
  clipped, over its last draw; a target with nothing pending is reused. Whole on first use,
  after `ResetEffect`, a failed draw, a new image or origin, and with `dirtyRegions` off.
  `GraphEvaluator::PreRenders()` (`draws`, `wholeDraws`, `rectsDrawn`, `pixelsDrawn`,
  `pixelsTarget`, `reused`, `failures`) and `PreRenderPending(producer, pin)`.
  Measured (WARP and the Adreno X2-90): a one-texel change into Nit Map feeding two
  Luminance Heatmaps draws 1 rect, 128 of 2,048 px, bit-identical to a fresh forced-whole
  context in every state tested. Real-content savings are pending: needs STAGE or real
  recordings.

### Changed

- **Channel Statistics 3, Luminance Statistics 7, Chromaticity Statistics 3** run the
  cell reduction (decision #258); a saved older version keeps its interleaved HLSL until
  upgraded. The parameter header changed, so effect blobs built for 1.0 do not register
  on 1.1 and the reverse (`EFFECTGRAPH_E_INCOMPATIBLE_EFFECT`); `packaging/Pack.ps1`'s
  run over the 1.0 DLL now checks that refusal and registers the 1.0 package's own blob.
- **A compute node whose input change was never dispatched is queued again** on the next
  frame, unless it is in error (decision #257): after a dropped queue, a failed dispatch,
  or a frame in which its input had no image. Before, it waited for the next change.
- **A ShaderLab pixel shader reads its image pins with a point filter**, and the root of
  an incremental realize is not cached by Direct2D (decision #242). Both made the same
  pixel come out differently depending on how it was drawn: Direct2D's interpolated
  texture coordinate, through a linear filter, blended a pixel with its neighbour by up
  to 8 codes at sharp edges when a draw was split into clipped rects (Gamut Map and Nit
  Map on a 256 x 144 target: 56 to 611 pixels a present), and a cached root rounded 1
  code differently on 6 of 2,048 pixels (Nit Map, Opacity 0.5). Lookup pins keep the
  linear filter; every existing test passes. The filter has its own switch,
  `EvaluatorOptions::pointImageFilter` (on; decision #246): off restores 1.0's linear
  filter whether or not `dirtyRegions` is on, so a filter regression can be told from a
  dirty-rect one. **ShaderLab:** on 1.1.0 its custom pixel shaders read image pins with
  the point filter; whole draws measured identical on the replay scenes, but a shader
  that samples an image pin between texels now snaps to the nearest one.
- **New host dimensions mark a custom effect's output changed**, so its consumers and a
  pending region see it even when the node was otherwise clean.
- **A root realized incrementally stays uncached** on every frame after, a `RealizeTo` of
  it included, through the new `FrameRequest::uncachedRoots` (decision #244). With
  `dirtyRegions` off nothing changes from 1.0: the root is cached, and the first
  incremental realize after the switch comes back draws whole. Before, the root was
  uncached only in frames that drew it incrementally, so the switch off did not restore
  1.0, and a host alternating `RealizeTo` and `RealizeToIncremental` flipped the root's
  cache state, and dropped its producers' intermediates, every frame.
- **`RegionDrawStats::draws` counts realizes that drew**; the new `realizes` counts every
  successful call, and `pixelsTarget` still accrues per call.

### Fixed

- **`GetCommittedValues` no longer reports S_OK for values consumers are not drawn with**
  (decision #269). After a whole dispatch, the deferred commit writes every non-Hosted
  field's texel at once and records it when the readback lands, one frame later. In that
  frame the call returned S_OK with the previous commit. It now returns S_FALSE with values:
  Hosted fields are current, and the fields the dispatch wrote still hold their last commit.
  S_FALSE with null still means not yet seeded. Found replaying a real DDA recording, where
  one present the OS reported whole was drawn with WhiteErrorBudget 0.5170 while the call
  said 0.4407. The replay harness now compares such frames against the texels, and
  `Stage3Stamp_CommittedPendingUntilLanded` / `_CommittedSettledAfterLanding` pin it.
- **The DDA replay cases skip void recordings in every stage.** Only stage 1 applied the
  void rule (more than 5% of presents accumulated, or a keyframe the rects did not rebuild),
  so stages 2 to 4 replayed a void recording. `RecordingIsVoid` makes its own pass, and every
  recording case calls it first.
- **`MapInvalidRect` on the engine's custom effects** (decision #266). A ShaderLab pixel
  shader answered identity on every pin; its lookup and analysis pins, and any pixel shader
  with a fixed output size, now answer infinite. The D3D11 compute bridge (identity) and the
  D2D compute effect (its input bounds) answer infinite. A null output pointer is
  `E_INVALIDARG`. Direct2D could otherwise keep a cached output behind a changed lookup
  table or diagram input. No switch.
- **A whole draw that fails after clearing the target forgets the target** (decision
  #244), so the next incremental realize draws whole instead of only the rects still
  pending over a cleared target.
- **A compute dispatch whose command list fails to close now fails.**
  `D3D11ComputeRunner::SubmitRecorded` ignored a `FinishCommandList` failure and the
  dispatch reported `S_OK`, so the node's change counted as consumed though nothing ran.
  With cell reductions that left the kept partials of the lost dispatch's cells stale for
  every later incremental dispatch. The dispatch now fails with the HRESULT (the node
  goes into error and its retry is whole), and the runner forgets its cell key, so its
  next dispatch recomputes all 64 cells whatever rects it is given.
- **`GraphEvaluator::DispatchesLastFrame()` is 0 on a frame that dispatched nothing.** It
  was reset only after `ProcessDeferredCompute`'s early returns (an empty queue, a queue
  dropped for a Source without output), so those frames reported the previous frame's
  count. `FrameExecutor` already masked this; a direct caller did not.
- **A graph with a bound video Time kept its inputs' rects.** `SettleVideoSources`
  re-marked every dirty node Whole after its frozen pass; it now restores each node's
  region.
- **Move bookkeeping stays bounded** (decision #265). The lists of moves handed to
  `MoveRects` were pruned only after a successful incremental realize, so a host that
  forwards moves but renders with `RealizeTo`, or whose only incremental root keeps
  failing, grew them without bound and paid a walk and a sort of the whole list on
  every `MoveRects`. They are now pruned after every `MoveRects` and every incremental
  realize, failed or not, and hold at most 1024 region generations
  (`Stage5_MoveBookkeeping`: at most 1025 held over 2500 calls on both paths). A root
  that reads only a static input no longer draws whole after 1024 generations of moves
  on another input. `RealizeToIncremental` with a `movedCapacity` above 0 and `moved` or
  `movedCount` null is `E_INVALIDARG` (it used to copy moves the host could not count).
  `movesCollapsed` now counts a move only where `drawn` covers its destination, so
  `movesApplied + movesCollapsed` falls short of the moves handed when one is dropped.

### Tests

- **Timing comparisons are paired** (decision #268). The hardware cost checks that
  failed now and then by under 1 ms (`Cost_CoverAll_NoDearerThanOff` and
  `_CpuNoDearerThanOff`, `SideBySide_ReduceCostNearCopy`, `Stage4Cost_*`,
  `MoveCost_*_CopiesCheaperThanToneMap`, `Stage0_FrameCpuAtTheFloor` and its loaded
  twin) time their arms interleaved, sample by sample with the order alternating, and
  judge them with `ComparePaired` (`Tests/TestCommon.h`, unit-tested by
  `TestPairedTiming`): a claim fails only when the paired differences contradict it
  with 99% confidence (a sign-test interval on their median), and each run prints that
  interval. The tolerances are unchanged. `SideBySide` times the reduction and the copy
  in one graph, both every frame (21 frames); `Stage4_CellCost` runs its nine
  configurations round-robin (21 rounds); `Stage0` adds an interleaved pass of the fix
  and the floor. The move-cost claim is asserted with `--plugin` only: the stand-in tone
  map costs about what the copies' submission does, so without the plugin it is
  reported. Adreno X2-90, Debug, raw paired differences (median, 99% interval): CoverAll
  GPU on - off +0.56 ms [-17.3, +16.7] on 70 ms presents, CPU -0.04 [-0.13, +0.12];
  reduce 3.51 ms, copy 1.72 ms, per-frame ratio median 1.75 (range 0.80-2.73, against
  the limit 4; measured in separate blocks the ratio had reached 4.22); 1 cell - whole -0.61 [-0.80, -0.44];
  whole - v6 whole -0.12 [-0.16, -0.09]; Scroll moves copied - drawn -2.61 [-3.11,
  -2.36]; Stage0 fix - floor, three runs, +0.30 to +0.33 ms [+0.23, +0.51] against an
  allowance of about 0.6 ms (Debug).
- **The D2D parity tests** (`Tests/Effects/D2DParityTests.cpp`, new). `TestD2DTableOracle`
  sweeps every registered built-in (defaults; each float and non-enum integer property at
  min, max and mid; every enum value; bools flipped; vectors at min and max; a scrambled color
  matrix; every input pin). A value Direct2D refuses is moved by bisection to the farthest one
  it takes, or the point fails as not probed, so no point silently probes the defaults; an
  allowlisted effect with properties must be swept away from them,
  invalidates a rect through Direct2D's own `InvalidateEffectInputRectangle` and reads
  `GetEffectInvalidRectangles`: no allowlisted CLSID is narrower than Direct2D at any point
  (17 effects, 148 probes, 0 narrower, 0 not identity; 31 others Whole), and every
  allowlisted one also changes 0 pixels outside the rect (232 render pairs, 168,192 px
  inside, 0 outside; a multi-input effect is rendered again over translucent inputs so no
  pin is hidden, and every pin must change some pixel), because Direct2D is not ground truth: it maps Emboss (116 px outside)
  and the six lighting effects (58) as identity. Cross Fade was dropped from the list: at
  Weight 0 or 1 Direct2D reports no rect for the unused input. Controls: a translate maps to
  (213,187)-(245,211), a sigma-20 blur widens to (145,125)-(288,262), a blur's footprint
  leaves the rect. `TestD2DEngineEffectInvalidation` checks each engine effect's answer
  through Direct2D, and a chain of engine pixel shaders and allowlisted built-ins (shader ->
  Color Matrix -> Composite pin 1 -> Saturation -> shader) keeps the rect, as `MapRegion`
  does; a blur in it widens. `TestEffectGraphPreRenderRegions` (`EffectGraphApiTests.cpp`)
  checks the pre-render regions against fresh forced-whole contexts, including rects
  parked on a consumer that is not needed until the next frame; its clip canary (a change
  left unreported stays undrawn) and disabling the union were each confirmed to fail it.
  A fault hook (`GraphEvaluator::FailNextPreRenderDrawForTest`) fills the target with
  garbage and fails the draw: the next use draws whole and equals a fresh render (dropping
  the reset to Whole fails all three checks); a producer reset and a resized producer output
  also draw whole. Rects parked on two frames are both drawn by the third frame's one
  dispatch; overlapping rects are drawn, and counted, as two; a producer pin serving
  another image at dispatch, with nothing invalidated, draws whole; and a producer whose
  image starts at (-11, -11) (a Composite over a soft-bordered blur) draws its rect at the
  right offset. Drawing only the first rect, dropping the origin offset and dropping the
  other-image check were each confirmed to fail these. Each pixel check follows a change into another Nit Map band, so a stale
  draw cannot pass it. `Incr_WholeThroughD2DBuiltIn` now uses Gaussian Blur (Color Matrix
  keeps rects), with `Incr_RectsThroughAllowlistedBuiltIn` and
  `Incr_AllowlistedBuiltInEqualsWhole` beside it, and the same pair through Composite pin 1;
  each first checks that the change showed inside the rect and nowhere else.

- **Move tests after the second review.** `ClientModel` applies each delivery's moves,
  then its dirty rects, to a CPU copy of the client's last image and must reach the
  target exactly; it replaces a metadata check that compared the stage simulator with
  itself (`_ClientModelCurrent`, with an instrument check that the same delivery
  without its moves does not reach it), and a frame held across presents gets rects
  only (`StageSim::Acquire`, `_HeldFrameGetsRectsOnly`). `MoveCase_VerifyDither1/2` run
  `verifyDirtyRegions` over a test-only root that dithers with `DitherQuantize` at
  strengths 1 and 2 (WARP: up to 1 and 2 codes off a fresh render): an honest move
  counts its drifted pixels as `verifyMoveDrift` and no mismatch, a later rect clears
  them, and a lie inside the moved pixels is a mismatch. `MoveCase_CopiedTwiceThroughScratch`
  counts the `CopySubresourceRegion` calls (two, none within one resource). The idle
  root's backlog is a chain of five moves, each reading the last, checked in order; a
  root that was not realized while a rect arrived draws the next move out of it
  (`MoveRepair_UnrealizedRootDrawsTheMove`); `MoveReplay`'s drift mask follows every
  draw that is not whole.

- **The move tests** (`Tests/Rendering/DdaReplayTests.cpp`, `Stage5_*`;
  `Tests/Graph/DirtyRegionTests.cpp`, `TestDirtyRegionMoves`). The simulated stage hands
  the engine each frame's moves (`StageSim::SetMoves`) and forwards `moved` with `drawn`.
  `Stage5_ReplaySynthetic` replays the six scenarios against a forced-whole render: the
  target before each present with the reported moves replayed on the CPU equals the target
  after it outside `drawn`; pixels differ from the forced-whole render only inside those
  moved since a draw last covered them, by at most `cMoveDitherCodes` (exact without
  the plugin's dither); `moved` is the frame's moves; the copied present draws exactly the
  dirty rects; both clients stay current; with `applyMoves` off and with a root that does
  not declare `shiftInvariant` every move is drawn and the target equals the forced-whole
  render. `Stage5_MoveCases`: a scroll whose revealed strip lies in the move's source stays
  a move; a second batch collapses the first; chained moves; a failed realize's rects
  under a move's source collapse it and a move reading its destination; a root errored on
  N, a move on N+1, recovery on N+2 draws the destination; more moves than `moved`
  holds; clipping; `verifyDirtyRegions` passes a true move and catches a lying one.
  `Stage5_MoveWhileCompilePending`, `Stage5_CommitWithMovesDrawsWhole` (whole, no
  copies, the move still reported), `Stage5_MoveCost` (both arms, wall clock as the
  instrument check on the GPU timestamps) and `Stage5_Stage3GateWithMoves` (the stage 3
  replay with moves: forwarded-whole rates unchanged, 0 of 25 surfaces differing on each
  scenario). `V2_MoveRectsNotImplemented` became `V2_MoveRectsAccepted`, and the C# smoke
  test's `V2_MoveRectsNotYet` became `V2_MoveRectsEmpty` / `V2_MoveRects`.

- **The cell-reduction tests** (`Tests/Rendering/DdaReplayTests.cpp`, `Stage4_*`):
  `Stage4_ReplaySynthetic` checks every field of the three statistics effects bit for
  bit against a context recomputing all 64 cells, over the seven synthetic scenarios,
  with a positive control (a change outside every reported rect must differ);
  `Stage4_ReplayRecordings` does the same over `--recording` (NOT RUN without it);
  `Stage4_Recovery` covers rects while the input has no output, a dropped queue, a failed
  dispatch and `RetryNode`, a lost submit, a bound `ClipNits` moving with a rect (the
  evaluator must hand Luminance Statistics no rects, not only the runner recompute 64
  cells), and the kill switch over a cell other than cell 0;
  `Stage4_RunnerLostSubmit` drives the runner alone through a lost submit and checks the
  next rect dispatch recomputes every cell; the replay checks Luminance Statistics' own
  cell mask against the rects it consumed on every present;
  `Stage4_CellMaskForRects` the cell bounds; `Stage4_CellCost` the cost by cells
  (timing assertions on hardware only: one cell under 0.8 x whole, 32 cells under
  0.9 x whole, and the legacy arm dispatched once a frame). The contract tests check `ReduceCells` against
  `SHADERLAB_REDUCE_CELLS` and the grid and `b2` slot against the HLSL; the stage 3
  replay reports its cell counters and checks that some dispatch recomputed fewer cells;
  the DDA tests check that the plugin's `dda_screenshot.json` saves Luminance Statistics
  at the catalog's version.
- **The deferred-commit tests** (`Tests/Rendering/DdaReplayTests.cpp`, `Stage3_*`):
  lag and drain, the stamp rule with two readbacks in flight, every policy with command
  lists on and off, the hosted API, seeding, a lane-3 binding added on a clean producer,
  a retry tick under `S_FALSE`, the synthetic replay with a poll-first stage, a
  decide-after-realize arm and its positive control, and `Stage3_DdaGraphPolicies`
  (`--plugin`: ShaderLab-private's `dda_screenshot.json`). `DdaStageSim` gains
  `DampedHostedDriver` and the `HdrHighlights` scenario. The API tests that expected
  `E_NOTIMPL` from the deferred methods now check `E_NOT_VALID_STATE` without the flag,
  `SetCommitPolicy` on an unbound property, and a context created with the flag.
  `Stage3_ReplayRecordings` needs `--recording` and is NOT RUN without it. Also: the kill
  switch by behaviour (no flag, and the flag with the readback skip off: hosted and
  threshold fields follow every dispatch), `IMMEDIATE` beside `THRESHOLD` in both
  command-list modes, the damped driver's decay, a failed copy pass, a first fill after
  the seed, a policy change after an early host commit, and `SetFloats` over a held drift.
  The replay compares every acquired client surface with a whole render of the committed
  values (and checks that comparison sees a wrong peak), requires every commit to be drawn
  whole by the sequence that made it, times ticks as well as presents and compares GPU
  totals with arm A, and asserts (hardware only) that Rects-present CPU time is within 25%
  + 0.1 ms of stage 2's. After review (decision #255): `Stage3_HostWriteOrdering`
  (`SetFloats` with a readback in flight, before and after the seed),
  `Stage3_ReadbackFailure` (`GraphEvaluator::FailNextReadbackPollForTest`),
  `Stage3_PolicyConflict` (a Gaussian Blur and the tone map disagreeing, then agreeing),
  `Stage3_DrainArmCommit` (a commit after realize N + 1 and one drain realize reach the
  next acquire as one whole rect equal to a forced-whole render), a seed slot older than
  the stamp, the `HOSTED` keep mask on a successful whole dispatch, the stamp's own slot
  checked silent, landing distances of 1 and 2 frames and a hosted commit's distance,
  and `TestCommitPolicyRoundTrip` (save, reload, an unknown tag). The replay asserts its
  instrument per scenario: a deferred readback on every Rects present, landings, landed
  values that move wherever the true statistics (a reference arm computed whole and read
  synchronously) move, landed values equal to the truth and the committed budget within
  its threshold of it, textures equal to records, and the drain arm's metadata and
  surfaces as the stage's. `GraphEvaluator::ResetDeferredCommitStats` scopes the
  landing-distance maximum to the replay.

- **The test runner takes `--plugin <dll>`**, an EffectGraph plugin it loads with
  `EffectGraphLoadPlugin` before the first case, for cases that need effects kept
  outside this repository. Without it those cases print `[SKIP]` and are counted
  under `NOT RUN`, apart from missing fixtures, so `--require-fixtures` does not fail
  them; a plugin that does not load stops the run (decision #236).
- **`Stage0_NoBlockingReadback`** (`Tests/Rendering/DdaReplayTests.cpp`), the first
  dirty-rects replay case, with ShaderLab-private's `PrivateEffects.dll`. It loads that
  repository's saved graphs, found by walking up from the plugin to a `graphs` folder
  (decision #237): `dda_screenshot.json`, the desktop-duplication tonemap stage's graph
  (texture input -> Luminance Statistics -> HDR Screenshot Tonemap (8bpc),
  `SourcePeakNits <- Max`, `WhiteErrorBudget <- WhiteErrorBudget`), and
  `adaptive_screenshot.json`, the graph it replaces, which bound `WhiteErrorBudget` to a
  Numeric Expression over `ClippedFraction` (decision #76). It checks each file's
  bindings, wires the host texture to nodes 2 and 4, and runs in the stage's
  configuration (no context flags: readback skip on, async readback and runtime compile
  off, the tone mapper from its plugin header). Both files come from one `graphs` folder;
  a missing one is a fixture skip (`--require-fixtures` fails it), and an API-built
  equivalent runs in its place. Every present changes the frame, and each present's
  readback is observed, not inferred: `Max` is poisoned before it and must come back as
  that present's peak. Two controls (the same graph with the skip off, and the old
  graph) must read back on every present, and do; a floor arm renders the same tone map
  with plain values while the statistics still dispatch. Every arm also runs with about
  5 ms of GPU copies queued before each present, where a blocking readback pays a drain.
  The profile's SDR white is 240 nits, a value no saved or default property holds, so
  the case sees Working Space's bindings resolve: both bound `SdrWhiteNits` / `ClipNits`
  read 240, and the shader's `ClippedFraction` matches a CPU count above 240 x 1.01 to
  six places. `WhiteErrorBudget` must equal the replaced expression's
  `min(2 x ClippedFraction, 20)`, and the file's `BudgetScale` / `BudgetMax` must be 2
  and 20. A present counts as a dispatch only if the statistics node has no runtime
  error, since the skip counter moves before the bridge's result is checked.
  Measured on the Adreno X2-90 (Release, 2880x1800, 40 timed presents per arm): the DDA
  graph dispatched the statistics on all 45 presents and read them back on none, no
  statistics field reached the CPU, and both bindings ran on the analysis texture (GPU
  bits 10). `RealizeTo` CPU medians, GPU idle: DDA 0.129 ms, floor 0.111, skip off
  0.417; GPU loaded: 0.096, 0.077 and 2.304 ms. The same-graph skip-off control is the
  readback's cost. The old graph (3.026 / 4.901 ms) also carries its v2 statistics
  shader, a single-group reduction the blocking `Map` waits on, and a White Level
  Adjustment, so its gap is not the expression's alone. Debug: DDA 1.100 / 0.997 ms
  against a floor of 0.724 / 0.660 and skip off 2.072 / 8.911. WARP (640x400): 0
  readbacks against 8 and 8. Timing checks, on hardware only: the fix below both
  controls, and at the floor (median within 1.6 x floor + 0.1 ms, idle and loaded).
  The tone map rendered byte-identical output to the same effect given the statistics'
  values and the profile's SDR white as plain properties, in two scenes whose Max (1000,
  4000 nits) and WhiteErrorBudget (0.8021, 0.9896) differ, and each value changed alone
  shows: of 20.7 M bytes, 0.79 M / 1.27 M differ with the budget halved, 123 K / 45 K
  with the peak halved, and 9.9 M / 8.7 M with SDR white at the saved 200.
- **The dirty-rect tests.** `DirtyRegionTests.cpp` (the region type, the 16-rect collapse
  keeping the oldest tag, identity through a pixel shader and Output, Whole through a
  blur, a lookup pin, a diagram, compute and a property edit, two paths' rects both
  kept); `TestLiveSourceDirtyRects`; `TestEffectGraphV2Interfaces` and
  `TestEffectGraphIncrementalRealize` (QI, `E_NOTIMPL`s, argument checks, the rect drawn
  and nothing else, equal to a whole render, every whole case, the bounding box past
  capacity, clipping to an image input, `verifyDirtyRegions`, the counters).
  `TestDirtyRegionChainEffects` checks that the replay's chain effects (Gamut Map, and
  the plugin tone map with `--plugin`) keep rects. The incremental-realize test leaves a
  texel outside the rect changed and unreported: the target keeps its old pixel there,
  and `verifyDirtyRegions` flags it; a fault-injected unclipped draw and a corrupted
  target fire the verifier's two checks (decision #245).
- **The replay harness** (`DdaReplayTests.cpp`, `DdaStageSim.{h,cpp}`, decision #243),
  with `--recording <dir>` for recordings from ShaderLab-private's new
  `tools/dda_recorder` ([format](docs/development/dda-recording.md)). Six synthetic
  scenarios (an idle caret between pointer-only frames, a scroll, an HDR video window, a
  window drag, rects covering the desktop, a video beside a scroll at 22 rects a frame)
  run through texture input -> tone map -> `RealizeToIncremental` -> a simulated stage
  with a client that acquires every present and one that holds across three, each
  present checked against a second context with `dirtyRegions` and output caching
  off. Every present must draw exactly the rects that changed (their bounding box past
  16, the whole target for a present with no metadata, which the idle-caret scenario
  sends every fifth present), and a sentinel written into the target outside them must
  survive: a draw with its clip removed fails that on every present of every rect
  scenario, and failed none of the earlier checks (decision #245). Every synthetic
  present changes visible content, so each one must change the target: a pipeline
  serving a stale frame to both arms passes the comparison but not that. On every scenario,
  WARP (480 x 270, 256 x 144) and the Adreno X2-90 (3840 x 2160, 256 x 144), with the
  built-in Gamut Map and with the plugin's HDR Screenshot Tonemap: no pixel outside the
  drawn rects changed, the target equalled the forced-whole render on every present,
  and every acquire of both clients equalled it with metadata covering every changed
  pixel. A root that fails for three presents times out every acquire meanwhile, and
  the first good realize, or a retry tick with no new rects, draws every pixel of the
  failed presents' rects. A realize without the flag that fails after clearing the
  target is followed by a whole draw. A root whose new shader compiles in the
  background returns `EFFECTGRAPH_E_COMPILE_PENDING` while rects arrive, times out the
  acquire, and the realize after the compile covers every waiting rect and equals the
  forced-whole render. A client on a second device, behind a keyed mutex, read 300
  acquires, each equal to the forced-whole render. Findings: the tone map's bytes match
  its own 8-bit grid exactly (0 of 74.6 M channels per scenario at 4K, recovered from an
  FP32 render, Quantize 1, every FP32 channel within 0.15 codes of the grid), and
  `CreateBitmapFromDxgiSurface` refuses an `_SRGB` bitmap over a
  `TYPELESS` texture with `E_INVALIDARG` on WARP and the Adreno. A synthetic recording
  written in the recorder's format replays to the same desktop, and its self-check
  flags a recording that leaves out one frame's rects (640 pixels); a present with no
  metadata is written as a keyframe and replays as Whole. A void recording is reported
  as a skip, and `--recording` with none replayed fails.
- **What it costs** (`Stage1_DrawCostByDirtyArea`, Adreno X2-90, Release, 3840 x 2160,
  median GPU time of a present, the realize plus the copy into the client, 40 presents,
  each from an idle GPU, `dirtyRegions` and `pointImageFilter` off (1.0's path: root
  cached, linear filter) / on, on the same frames). Instrument: the input's and the
  root's `OutputChangeCount` moved on every present in both arms, the switch-off arm drew
  exactly its target's pixels, and every present of the on arm drew exactly what
  changed. Gamut Map: scroll (0.375 of the target) 1.567 / 0.615 ms, HDR video (0.25)
  1.573 / 0.435, window drag (0.111) 1.542 / 0.247, video and scroll (0.744) 1.547 /
  1.149, rects covering everything 1.539 / 1.544. HDR Screenshot Tonemap, same order:
  6.361 / 2.192, 21.437 / 18.376, 3.716 / 0.399, 22.529 / 21.518 and 70.082 / 66.702.
  The idle caret is two kinds of present, so its median shows only one: it draws 0.182
  of the target, because every fifth present arrives with no metadata and is drawn
  whole. Its 32 presents with rects cost 1.547 / 0.066 ms (Gamut Map) and 8.322 / 0.035
  ms (tone map); its 8 with no metadata 1.565 / 1.506 and 8.427 / 8.347; the mean over
  all 40 is 1.547 / 0.353 and 8.555 / 1.812. An earlier report gave the idle caret's
  drawn share as 0.0000; that was wrong. The realize's CPU time (median, same presents)
  with rects covering everything: 0.268 / 0.265 ms (Gamut Map), 0.281 / 0.293 ms (tone
  map); the region bookkeeping costs no measurable CPU there. One run each.
  The tone map's cost follows content, not area: its HDR video pixels cost far more than
  the SDR desktop, so the video quarter is most of a whole frame's cost. A frame whose
  rects cover the target draws it whole and costs what the switch off costs. Past 16
  rects the bounding box adds 30.7% to the rects' union in the video-and-scroll
  scenario. Numbers from real recordings are pending the recordings.
- **The gate's tests.** `TestEffectGraphAnalysisGate` (`EffectGraphApiTests.cpp`, readback
  skip off): a rect on Luminance Statistics' texture that leaves `Max` bit-identical
  gives an `unchanged` verdict, a dispatch input region of exactly that rect, the
  consumer reading the same texture draws only the rect, and a consumer drawn from
  another texture has an empty region, draws nothing and keeps its `OutputChangeCount`;
  a rect that raises `Max` redraws both whole and moves the count; with the gate off the
  same unchanged rect wakes both. A `ClipNits` edit on the frame of a rect, and a
  `ClipNits` bound to Working Space whose SDR white moves on that frame, each dispatch
  whole and give a `ClippedFraction` bit-equal to a fresh forced-whole context's (0.440918;
  0.557617 -> 0.242188). `TestEffectGraphNoFallback`: a blob without the GPU-bound
  build under `NO_FALLBACK` fails `RealizeTo` and `RealizeToIncremental` with
  `EFFECTGRAPH_E_FALLBACK_BUILD`, leaves both targets as they were and the region
  pending; without the flag it renders, counting 2 fallback builds and a fallback
  readback; a blob with the build renders with none after its first frame.
  `Propagate_WholeIntoCompute` became `Propagate_ComputeInputRegionKeepsRects` and
  `Propagate_WholeOutOfCompute`; `V2_NoFallbackFlagNotImplemented` became
  `V2_NoFallbackFlagAccepted`.
- **`Stage2_GateReplaySynthetic`** (`DdaReplayTests.cpp`): the DDA graph over the six
  synthetic scenarios in three arms on the same frames -- gate on with the readback skip
  off (the verdicts and change rates), a forced-whole reference, and gate on with the
  skip on (the stage's configuration, timed) -- with the HDR Screenshot Tonemap under
  `--plugin`, a per-pixel stand-in bound the same way without. Every present in every
  arm: no fallback readback, both gated targets equal to the reference, the input and
  chain end changed, one verdict per present; a present judged `unchanged` drew exactly
  the changed rects. Adreno X2-90, Release, 3840 x 2160, 24 presents, the plugin's tone
  map. Skip off, presents judged unchanged / changed / whole: idle caret 20 / 0 / 4,
  scroll 24 / 0 / 0, HDR video 24 / 0 / 0, window drag 18 / 6 / 0, covering rects
  0 / 0 / 24, video and scroll 24 / 0 / 0. `Max` kept its bits on every present of every
  scenario; `WhiteErrorBudget` on every present except window drag's (0.750). On WARP
  (480 x 270, 18 presents) the small frame moves them more: `Max` 0.944 and
  `WhiteErrorBudget` 0.833 in HDR video and video and scroll, `WhiteErrorBudget` 0.722 in
  window drag. Synthetic content keeps its peak in view, so these say little about a real
  desktop; recordings are pending. With the skip on, the gate judges nothing (every rect
  present is `noValue` and redraws whole) until deferred commit: the realize CPU median
  of a present with rects, gate and skip on, is 0.330 ms (idle caret), 0.333 (scroll),
  0.377 (HDR video), 0.224 (window drag), 0.339 (video and scroll); covering rects 0.381
  ms; an earlier run of the same build gave 0.177-0.383 ms, so treat single-run medians
  as +-0.15 ms. `Stage2_GateReplayRecordings` replays `--recording` the same way.

## [1.0.2] - 2026-10-21

### Fixed

- **A pixel shader that reads analysis values on the GPU no longer costs their
  producers a CPU readback on the first frame.** With
  `skipUnneededCpuReadback` on, a producer skips its readback when every
  consumer reads it on the GPU, but a pixel shader was judged by the build it
  last ran, and its build is chosen after the dispatches. On a fresh graph that
  was always the baseline, so each producer copied its results to staging and
  blocked on a `Map` that nothing read. A pixel shader is now judged by the
  build it will get (the binding plan with the producers that publish this
  frame, and a ready build for it); after the dispatches the evaluator checks
  again and reads back late for a consumer that will not get it, so a miss
  costs the old readback rather than a wrong value (decision #235). Measured on
  WARP (`TestLane3NoCpuReadback`): the first frame of a fresh graph dispatched
  once and read back 0 times, down from 1, with the pixel shader on its texture
  build; three unchanged frames dispatched nothing; a changed input dispatched
  once with no readback.
- **A CPU consumer bound to a producer whose values were on the GPU only read
  stale values.** Once a producer had skipped a readback, its fields kept the
  last values that reached the CPU, and a binding added later (or a consumer
  falling back to its cbuffer) read those until the producer next dispatched:
  in the test, 160 where the producer had measured 400. The evaluator now
  records that the fields are behind and reads the results when a binding or a
  host hint needs them, without a dispatch. The same record stops an
  analysis-only node whose results never reached the CPU from being
  re-dispatched every frame for having no fields.

## [1.0.1] - 2026-10-20

### Fixed

- **A pixel shader bound to an analysis value now reads it on the GPU from the
  first frame.** The producer's first frame asked its compute runner for the
  analysis texture before the bridge had initialized the runner, which dropped
  the request (`RequestAnalysisTexture` returned `E_NOT_VALID_STATE`). The
  consumer fell back to the CPU readback and its constant buffer, which gave the
  right value but cost a blocking readback a dispatch, and a graph that then
  stayed unchanged never asked again, so it stayed there until something
  dirtied the producer. The runner now records the request whatever its state,
  and `RequestAnalysisTexture` always returns `S_OK`. Measured on WARP with a
  graph built once and never dirtied: the consumer's applied binding mode was
  cbuffer on all four frames before, texture on all four after, with the pixel
  equal to the measured Max (80.0082) both ways. Every earlier lane 3 test
  dirtied the graph each frame, which hid this; `TestLane3StaticGraph` does not.
- **C4702** in `ShaderCompiler.cpp`: the final `return false` of a `std::visit`
  `if constexpr` chain was unreachable for the types handled above it; it is now
  the chain's `else`.
- **The release SBOM scan skips `packaging/`.** It read our own `.nuspec`
  templates as dependencies and tried to parse the SBOM it was writing. The SBOM
  still lists the build's dependencies from `packages.config`.

## [1.0.0] - 2026-10-18

### Added

- **Releases from a tag** (decision #233): `release.yml` runs CI, checks the tag
  against `EffectGraphVersion.h`, packs with the v143 toolset (`Pack.ps1
  -PlatformToolset`), writes an SPDX SBOM and build-provenance attestations, and
  pushes to nuget.org after approval through trusted publishing. Actions are
  pinned to commits. See [Releasing](docs/development/releasing.md).
- **`CONTRIBUTING.md`**: contributions are MIT-0, and no third-party code.
- **Live source kinds** (`Effects/LiveSource.h`, decision #234): a host registers a
  kind of Source whose frames it produces (a capture, a camera), and
  `SourceNodeFactory` opens it, ticks it (`TickLiveSources`), reopens it once a
  second when it is lost and drops it with its node. `SetLiveSourceTarget` hands a
  source what it opens on, and `RegisterNodeMaker` adds names to
  `CreateNodeByName`. Tested with a scripted fake source (`LiveSourceTests`).
  Screen and window capture, which ShaderLab's engine had built in, stay in
  ShaderLab as two such kinds.
- **Its own repository** (decision #231), split out of ShaderLab with the history of
  its files. The engine library is now `EffectGraphEngine.lib`
  (`EffectGraphEngine.vcxproj`, formerly `ShaderLabEngine`), and the solution is
  `EffectGraph.slnx`.
- **The public API:** `EffectGraph.dll` with a seven-function C API and COM
  interfaces (contexts, nodes, texture inputs, values and values buffers), effect
  blobs built at compile time by `EffectGraphCompile`, every built-in as a header,
  effect plugins (`EffectGraphPlugin.h`), and its version in `EffectGraphVersion.h`.
- **Packages:** `EffectGraph.Native`, the C# `EffectGraph` bindings and
  `EffectGraph.Engine`, built by `packaging/Pack.ps1`.
