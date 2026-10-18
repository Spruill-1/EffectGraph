# CLAUDE.md

EffectGraph — a Direct2D effect graph engine for D3D11 programs in an HDR / WCG
pipeline, as a static library, `EffectGraph.dll` with a C/COM API, C# bindings and
NuGet packages. [ShaderLab](https://github.com/Spruill-1/ShaderLab) is the tool built
on it. The deep reference is [`docs/`](docs/README.md).

## Hard rules

- **C++/WinRT only** for the engine, the DLL and the tools. The exception is
  `bindings/`: the C# bindings over the DLL's C API and COM interfaces, and their test.
- **Every new `.cpp` starts with `#include "pch_engine.h"`.** Precompiled headers are
  mandatory.
- **Nothing ShaderLab-specific.** No MCP, no UI, no third-party source (EffectGraph is
  MIT-0 and contains none). A host adds what it needs through the engine's hooks
  (`Rendering/MathExpression.h` for expressions, the plugin host for effects).
- **The public API is frozen by IID.** `Engine/Api/EffectGraph.h` and
  `EffectGraphPlugin.h` are C-compatible; a shipped interface never changes in place,
  a new one gets a new IID. Versions are in `Engine/Api/EffectGraphVersion.h`.
- **Docs are part of the change.** A significant change updates the relevant file
  under `docs/`, adds a `CHANGELOG.md` entry, and — when the *why* isn't obvious from
  the code — a row in [`docs/history/decision-log.md`](docs/history/decision-log.md).

## Build, test

Windows only, x64 and ARM64; outputs in `<Platform>\<Config>\<Project>\`. Find
MSBuild with `vswhere`; **on an ARM64 machine use `MSBuild\Current\Bin\arm64\MSBuild.exe`**
(the default one is 32-bit and picks a compiler that runs out of memory; see
[`docs/development/build.md`](docs/development/build.md)). The Bash tool reports the
wrong architecture on ARM64 hosts; branch on arch in PowerShell.

```pwsh
nuget restore EffectGraph.slnx -SolutionDirectory .
$vs  = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
         -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
& "$vs\MSBuild\Current\Bin\MSBuild.exe" EffectGraphTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:m /nologo
& '.\x64\Debug\EffectGraphTests\EffectGraphTests.exe' --adapter warp   # ends "ALL <n> TESTS PASSED"
pwsh -File packaging\Pack.ps1                                             # packages + C# smoke test
```

Pipeline is **always scRGB FP16 linear light** (1.0 = 80 nits), no format switching,
no built-in tone mapping: tone mappers are effects.

## Effect-authoring traps that cost real debugging time

More in [`docs/architecture/d2d-d3d11-hybrid-compute.md`](docs/architecture/d2d-d3d11-hybrid-compute.md).
The ones that bite most often:

- **Evaluation runs with no draw session open on its device context.**
  `GraphEvaluator::Evaluate` and `ProcessDeferredCompute` each open a session only
  for their own drawing (an analysis readback, a compute input the shader cannot
  read in place) and close it, so calling either inside a `BeginDraw` on the same
  context nests and fails with `D2DERR_WRONG_STATE` (`0x88990001`); a compute
  input render that fails fails its node. The post-dispatch sweep runs as
  `Evaluate(EvalMode::Frozen)` so it cannot re-queue a compute node.
  `Rendering::FrameExecutor::Run` owns the order, opens a session only for the
  host's draw, and every host and test renders through it — call it, don't
  re-sequence the passes yourself. Until 2026-10-16 the frame ran inside one
  session and `ProcessDeferredCompute` *required* it (outside one its `DrawImage`
  silently no-oped and compute read black); decision #221. History worth knowing:
  `Evaluate` used to pre-render compute inputs too (`PreRenderInputBitmap`, deleted
  2026-09-25), and getting the split wrong looked like a property-plumbing bug —
  correct on the first frame, then black or **stale** after the first `set-property`.
  Compute inputs are now pre-rendered only inside `ProcessDeferredCompute`, after
  any same-pass producer has dispatched (decision #95).
- **D2D→D3D11 texture handoff needs the draw submitted** — `EndDraw`, or an
  explicit `dc->Flush()` inside a session — between `DrawImage` and any D3D11 read,
  or D3D11 reads zeros.
- **New D2D custom effects need two evaluation passes** before output is correct.
- **`D3DCOMPILE_WARNINGS_ARE_ERRORS` optimizes out cbuffer vars** not referenced on
  *all* paths — read every cbuffer var at the top of `main()` before branching.
- **A D2D pixel shader MUST declare `SCENE_POSITION`, and `TEXCOORD` is then
  normalized.** The required input signature is
  `main(float4 : SV_POSITION, float4 : SCENE_POSITION, float4 : TEXCOORD0, …)`,
  one TEXCOORD per input. Omit the middle parameter and the binding shifts: what
  you called `TEXCOORD0` receives the **scene** coordinate instead of input 0's
  texel coordinate. Those two coincide only while the content fills D2D's
  intermediate allocation, so the mistake is invisible at the usual source size
  and then displaces the image by `−(intermediate − content)/2` per axis the
  moment it isn't — measured at exactly −128 px for a 256 px source in a 512 px
  intermediate. This file previously claimed "TEXCOORD is pixel/scene space, not
  normalized"; that was a description of the bug, not of D2D. With the signature
  correct, sample with `Sample(s, uv0.xy)`, take pixel coordinates from
  `SCENE_POSITION`, and give each input its **own** TEXCOORD — a two-input effect
  reusing one coordinate for both is how "two-input effects render displaced"
  was born. **And `SCENE_POSITION` is not exact:** D2D interpolates it with up
  to 2^-12 px of error, and the error *moves when D2D re-tiles the effect* --
  which wiring an extra input can trigger. Anything chaotic in it (a dither
  hash) must `floor()` it to the integer pixel first, or the same pixel renders
  differently depending on graph topology (measured: 0.037% of pixels, 1-3 codes).
- **scRGB is signed on purpose.** Negative Rec.709 components are how wide-gamut
  color is expressed (BT.2020 green ≈ `(-0.87, 1.00, 0.06)`). A `max(rgb, 0)` or
  `saturate()` at the top of a color transform is a **gamut clip**, not a safety
  net — this exact bug silently sRGB-clipped the whole ICtCp suite. Use the signed
  PQ helpers in `Effects/ColorMath.cpp`.

## Conventions

Namespaces mirror directories (`ShaderLab::Graph`, `::Rendering`, `::Effects`,
`::Engine`; the `ShaderLab::` root predates the split and stays). Members are
`m_`-prefixed, methods/types PascalCase, locals descriptive camelCase. Constants take
a `c` prefix, statics `s`, static consts `sc` (`cMaxOptionValues`, `scGamutLutWidth`);
older code still uses `k`, so don't mass-rename it in passing. Comments are short and
plain: what the next block does, or the one-line constraint that would surprise a
reader. Change history and measurements belong in `CHANGELOG.md` and the decision log,
not in code. COM members are `winrt::com_ptr<T>`; custom
D2D effects hand-roll `IUnknown` refcounting on a `LONG m_refCount`. Init paths use
`winrt::check_hresult`; hot paths use `SUCCEEDED`/`FAILED` with early return.

New files go in the matching directory **and** into both the project
(`EffectGraphEngine.vcxproj`, or the right one) and its `.vcxproj.filters`.

**Change a node through a setter, not its fields.** `Graph/GraphEdits.h` has one per
kind of change (`SetNodeProperty`, `SetNodeMedia`, `RenameNode`, `SetNodePosition` /
`MoveNodes`, `MarkNodeChanged`, `RetryNode`, `ResetNodeOutput`, `ReplaceCustomEffect`,
`SetBindingComponent`), each in a node form and a (graph, id) form, and each marks the
node dirty exactly when the change affects what it renders. Untyped values (a host's
scripting or MCP layer) go through `Graph::WriteProperty`. Building a fresh node before `AddNode` may
set fields directly (`Effects::CreateCustomEffectNode`, `EffectRegistry::CreateOutputNode`
and `Graph::NewInstanceOf` cover the common shapes), and a white-box test may stage
engine-owned state (`dirty`, `needed`, `runtimeError`, `cachedOutput`, `clockTime`).

**Don't add a bare `catch (...) {}`.** Record the failure somewhere a caller can see
it (`LastError()`, `runtimeError`, an MCP status field), or — when swallowing really is
correct (teardown, a best-effort UI indicator) — say so in a comment, so an intentional
swallow is distinguishable from an oversight. Of ~37 catch-alls in the engine
directories, the genuinely silent ones have been fixed or annotated; two cost real
debugging time before that: `DisplayMonitor::Initialize` swallowed a throw and served
SDR defaults on a 4000-nit HDR panel (a missing `Windows.System.DispatcherQueue`, not
an API bug), and `OutputWindow::SaveImageAsync` swallowed every WIC/D2D failure so
"Save image" silently did nothing.

## Verifying work

Claims about color or performance in this project are expected to be **measured**,
not asserted — that standard is visible throughout `CHANGELOG.md` and is the house
style. Before reporting a fix: run the tests, and where the change is numeric, read the
pixels back (a test, or ShaderLab's headless `--pixels`) and quote the numbers.

**Validate the instrument before you believe the number.** A whole measurement
round here once produced a complete, plausible, *wrong* table — twice over, from
two independent silent faults: compute nodes serving stale pixels, and a metric
chain (`Delta E Comparator` → `Luminance Statistics`) that was actually measuring
the luminance of a heatmap, because the comparator is image-output only and has no
analysis fields. Neither was visible in the numbers. So:

- Prove the pipeline is live before sweeping it — change an input, confirm the
  output moves. A sweep that returns identical values at several settings is
  **suspect**, not a finding that "the parameter has no effect".
- Watch stderr. Nested-draw and pre-render failures now print `[ShaderLab]` lines;
  any of them voids that run's numbers.
- Prefer FP32 readback plus CPU math over reading a diagnostic effect's rendered
  output, and keep the meter off the compute path being tested.

One more shape to watch: **statistics are permutation-invariant**. Min/Max/Mean/
Median/P95 match perfectly across a change that scrambles pixel *positions*, so a
reduction alone cannot validate anything spatial — pair it with scattered
pixel probes or a capture diff.
