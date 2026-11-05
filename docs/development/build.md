# Building EffectGraph

Windows only; x64 and ARM64, Debug and Release. Outputs land in
`<Platform>\<Configuration>\<Project>\`.

## Prerequisites

- Visual Studio 2022 or later with the **Desktop development with C++** workload and
  the MSVC tools for each platform you build (x64, ARM64).
- A Windows 10 SDK (10.0.22621 or later) and the `nuget` CLI.
- For `packaging/Pack.ps1`: the .NET 8 SDK (C# bindings and their smoke test).

```pwsh
nuget restore EffectGraph.slnx -SolutionDirectory .
```

restores the two NuGet packages the C++ projects import (C++/WinRT and the SDK build
tools) into `packages\`.

## Projects

| Project | Output |
|---|---|
| `EffectGraphEngine.vcxproj` | `EffectGraphEngine.lib`: the engine, linked statically by everything below |
| `BuiltInEffects.vcxproj` | `BuiltInEffects.lib`: the built-in effect table `EffectGraphCompile --builtins` reads |
| `EffectGraphCompile.vcxproj` | `EffectGraphCompile.exe`: builds effect blobs and headers |
| `EffectGraph.vcxproj` | `EffectGraph.dll`, `EffectGraph.lib`, and `include\` with the public headers and a header per built-in |
| `TestEffectPlugin.vcxproj` | A plugin DLL built the way a real one is, for the tests |
| `EffectGraphTests.vcxproj` | `EffectGraphTests.exe`, the test runner |

`build\EffectGraphBuiltIns.targets` runs `EffectGraphCompile --builtins` before C++
compiles, writing the built-in headers to `<Platform>\<Configuration>\EffectGraphBuiltIns\`.
When the machine cannot run the target platform's tool (ARM64 built on x64), it
builds and uses the x64 tool; the headers are the same on every platform.

## Building and testing

```pwsh
$vs  = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
         -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msb = "$vs\MSBuild\Current\Bin\MSBuild.exe"            # x64 host
# $msb = "$vs\MSBuild\Current\Bin\arm64\MSBuild.exe"    # ARM64 host (below)

& $msb EffectGraphTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:m /nologo
.\x64\Debug\EffectGraphTests\EffectGraphTests.exe --adapter warp   # ends "ALL <n> TESTS PASSED"
```

`--adapter warp` runs on the software rasterizer, so no GPU is needed; CI runs it.
The runner also takes `--tag <fast|gpu|video|timing|math|...>`, `--filter <text>`,
`--list` and `--require-fixtures` (a missing fixture fails instead of skipping).

Timing assertions (`TIMING_TEST`) count only on hardware outside CI;
`SHADERLAB_TIMING_TESTS=1` or `0` overrides. A cost comparison between two arms
times them interleaved, sample by sample with the order alternating, so a GPU clock
change lands in both, and asserts with `ComparePaired` (`Tests/TestCommon.h`): it fails
only when the paired differences contradict the claim with 99% confidence (a
distribution-free sign-test interval on their median), and prints that interval as
`[info] ... interleaved pairs` so each run reports its own spread.

`--plugin <path>` loads an EffectGraph plugin (`EffectGraphLoadPlugin`) before the
first case, for cases that need effects kept outside this repository: the dirty-rects
replay cases in `Tests/Rendering/DdaReplayTests.cpp` take ShaderLab-private's
`PrivateEffects.dll` for the HDR Screenshot Tonemap. A plugin that fails to load stops
the run. Without the option those cases print `[SKIP]` and count under `NOT RUN`, apart
from missing fixtures, so `--require-fixtures` does not fail them.

`Stage0_NoBlockingReadback` also loads the plugin repository's saved graphs,
`graphs/dda_screenshot.json` and `graphs/adaptive_screenshot.json`, from the nearest
`graphs` folder walking up from the plugin DLL that holds `dda_screenshot.json` (the
repository root, for a plugin in its build output), both from that one folder. A
missing file is a fixture skip, which `--require-fixtures` fails, and an API-built
equivalent of that graph runs in its place.

```pwsh
.\ARM64\Release\EffectGraphTests\EffectGraphTests.exe --adapter default --filter Stage0 `
    --plugin ..\ShaderLab-private\ARM64\Release\PrivateEffects\PrivateEffects.dll
```

The plugin must be built against the same shared HLSL headers as the engine under
test (a blob built against others is refused as incompatible), so a plugin built
from the published package fits only an engine at that version.

`--recording <dir>` names a folder of desktop-duplication recordings, one subfolder
each, made by ShaderLab-private's `tools/dda_recorder` ([DDA recordings](dda-recording.md)).
`Stage1_ReplayRecordings` replays each one through the dirty-rect stage simulator with
the same checks as the synthetic scenarios, and skips a void one. Without the option it
prints `[SKIP]` and counts under `NOT RUN`; every other `Stage1_` case runs on
synthetic frames made in the process. With `--plugin` the replay chain is the HDR
Screenshot Tonemap, without it the built-in Gamut Map.

**Building ARM64 on an ARM64 machine needs the ARM64 MSBuild.**
`MSBuild\Current\Bin\MSBuild.exe` is 32-bit; under emulation it reports
`PROCESSOR_ARCHITECTURE=x86`, toolset selection falls through to the 32-bit
`HostX86\arm64\cl.exe`, and that compiler runs out of address space on the large
translation units (`C3859: Failed to create virtual memory for PCH`, then `C1076`).
`/p:PreferredToolArchitecture=x64` does not help: the props file declares it
`TreatAsLocalProperty` and demotes it back. Use `MSBuild\Current\Bin\arm64\MSBuild.exe`.
CI cross-compiles ARM64 from x64 runners, where this never shows, so it also has a
native ARM64 job.

## Packages

[packaging.md](packaging.md).
