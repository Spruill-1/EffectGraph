# Changelog

All notable changes to EffectGraph are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions follow
`Engine/Api/EffectGraphVersion.h`. EffectGraph was developed inside
[ShaderLab](https://github.com/Spruill-1/ShaderLab) until 1.0.0; that history is in
ShaderLab's changelog and in [the decision log](docs/history/decision-log.md).

## [1.0.0] - Unreleased

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
