# EffectGraph Documentation

The repo root [README.md](../README.md) covers what EffectGraph is and how to build
it. [`CHANGELOG.md`](../CHANGELOG.md) tracks releases; the
[decision log](history/decision-log.md) records architectural choices with their
reasons. EffectGraph grew out of [ShaderLab](https://github.com/Spruill-1/ShaderLab),
and the log's earlier entries are that history.

## Using EffectGraph

- [EffectGraph API](architecture/effectgraph-api.md) — `IEffectGraphContext`, `IEffectGraphNode`, texture inputs, `Realize` / `RealizeTo`, shipping your own effects, plugins, analysis values on the GPU, versions.
- [Packages](development/packaging.md) — `EffectGraph.Native`, the C# `EffectGraph` bindings and `EffectGraph.Engine`: what they hold and how they are built.
- [Building](development/build.md) — prerequisites, projects, tests, the ARM64 toolset trap.
- [Releasing](development/releasing.md) — tag-driven publishing to nuget.org, the v143 toolset, SBOM and provenance, one-time setup.

## How it works

- [Pipeline Format](architecture/pipeline-format.md) — why the pipeline is always scRGB FP16.
- [Graph Model](architecture/effect-graph-model.md) — `GraphModel`, `EffectNode`, `EffectEdge`, `PropertyValue`, JSON, dirty tracking.
- [Topological Evaluation](architecture/topological-evaluation.md) — evaluation order and cycle detection.
- [Compute Analysis Pipeline](architecture/compute-analysis-pipeline.md) — D2D compute conventions, readback, analysis output.
- [D2D / D3D11 Hybrid Compute](architecture/d2d-d3d11-hybrid-compute.md) — `CustomComputeBridgeEffect`, `D3D11ComputeRunner`, GPU-binding routing.
- [Display Monitoring](architecture/display-monitoring.md) and [Display Profile Mocking](architecture/display-profile-mocking.md) — the display a graph targets, real or simulated.

## Effects

- [Built-in Effect Catalog](effects/builtin-catalog.md)
- [User Effects](effects/user-effects.md) — effects registered at runtime from files.
- [Effect Versioning](effects/effect-versioning.md)
- [Option Variants](effects/option-variants.md)
- [Parameter Nodes](effects/parameter-nodes.md) and [Numeric Expression](effects/numeric-expression.md)
- [Property Bindings](effects/property-bindings.md)
- [Working Space](effects/working-space.md)
- [New Effect Defaults](effects/new-effect-defaults.md)

## History

- [Decision Log](history/decision-log.md)
