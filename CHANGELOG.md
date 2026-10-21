# Changelog

All notable changes to EffectGraph are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions follow
`Engine/Api/EffectGraphVersion.h`. EffectGraph was developed inside
[ShaderLab](https://github.com/Spruill-1/ShaderLab) until 1.0.0; that history is in
ShaderLab's changelog and in [the decision log](docs/history/decision-log.md).

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
