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
