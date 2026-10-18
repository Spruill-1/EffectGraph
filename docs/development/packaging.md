# EffectGraph Packages

EffectGraph ships as three NuGet packages (decisions #223, #228), built by
[`packaging/Pack.ps1`](../../packaging/Pack.ps1) into `packaging/out`:

| Package | What it holds | For |
|---|---|---|
| `EffectGraph.Native` | `EffectGraph.dll` for x64 and ARM64 (`runtimes/win-*/native`), `EffectGraph.lib` per platform, `EffectGraph.h` and a header per built-in effect (`build/native/include`), every built-in as an `.effectblob` (`effects/`), `EffectGraphCompile` for x64 and ARM64 (`tools/`), and MSBuild targets | C++ projects, and anything that loads the DLL itself |
| `EffectGraph` | C# bindings (assembly `EffectGraph.Interop.dll`, net8.0, Windows only) with every built-in embedded; depends on `EffectGraph.Native` | .NET projects |
| `EffectGraph.Engine` | The engine as a static library (`EffectGraphEngine.lib` for x64 and ARM64, Debug and Release), its 77 headers in their folders, `pch_engine.h`, every built-in effect header, and targets that add the include path, define `EFFECTGRAPH_STATIC` and link the library | ShaderLab, and any program working inside the engine; not a stable API |

## Building them

```pwsh
pwsh -File packaging\Pack.ps1              # version from Engine/Api/EffectGraphVersion.h
pwsh -File packaging\Pack.ps1 -Version 1.0.1-preview.1 -SkipBuild
```

It builds `EffectGraph.vcxproj` and `EffectGraphCompile.vcxproj` in Release for x64 and ARM64 (the DLL project generates the built-in headers and blobs), and `EffectGraphEngine.vcxproj` in Debug and Release for both with `EffectGraphPackageBuild=true`, stages and packs `EffectGraph.Native` and `EffectGraph.Engine` with `nuget pack`, packs the C# bindings with `dotnet pack` against it, then runs the test below. Restores go to `packaging/obj/packages` rather than the global cache, which would keep an earlier pack of the same version.

## EffectGraph.Engine

A program that works inside the engine, as ShaderLab does, uses this package rather than the DLL (decision #228). Its headers are not a stable API: pin the version.

- **Which headers:** every `.h` in `Graph/`, `Rendering/`, `Effects/` and `Engine/`, in their folders, so the engine's own relative includes work. Consumers include them from the include root (`#include "Graph/GraphModel.h"`). The generated built-in headers sit at the root; they include `EffectGraph.h` by bare name, so the targets also put `include\Engine\Api` on the path rather than shipping a second copy, which `#pragma once` could not tell from the first.
- **How the library is built:** `EffectGraphPackageBuild=true` builds into `<Platform>\<Config>\EffectGraphEngine.Package\` without `/GL` (an LTCG library links only with the exact compiler that built it) and with debug information in the objects (`/Z7`), so a consumer's linker finds it without a `.pdb` path from the build machine. That makes the package 90 MB (the Debug libraries are 180-190 MB, Release 90 MB, uncompressed), within nuget.org's 250 MB limit.
- **Precompiled header:** a consumer compiles its own `.cpp` with `#include "pch_engine.h"` and `/Yc`.
- **Linking:** the package's targets define `EFFECTGRAPH_STATIC` and link `EffectGraphEngine.lib` for the project's platform and configuration; a project that only compiles against the headers (a static library of its own) sets `EffectGraphEngineHeadersOnly` to `true`. The consumer needs C++/WinRT (`Microsoft.Windows.CppWinRT`) and C++20.
- **Building against a checkout instead:** ShaderLab can point at an EffectGraph working tree to change both at once; see its `build/EffectGraphEngine.targets`.

## The test

[`bindings/csharp/EffectGraph.Smoke`](../../bindings/csharp/EffectGraph.Smoke/Program.cs) is a minimal C# console program that references only the packed `EffectGraph` package: it creates a WARP D3D11 device by P/Invoke, an FP16 texture through a three-method `ID3D11Device` declaration, registers the embedded Luminance Statistics, and checks the statistics of a 3.0 scRGB texture (240 nits) through `GetValues` and the values buffer, a write to the buffer, and that an unregistered built-in fails with `HResults.EffectNotIncluded`. `Pack.ps1` fails if it does.

## C++ consumers

`build/native/EffectGraph.Native.targets` adds the include path and `EffectGraph.lib` for an x64 or ARM64 project (any other platform is an error), copies `EffectGraph.dll` beside the output, and imports `EffectGraph.targets`, so `EffectGraphEffect` items compile a project's own effects with the package's `EffectGraphCompile` (the ARM64 tool on an ARM64 machine, else x64). A built-in is included by its header, which registers it at startup.

## C# consumers

The assembly is `EffectGraph.Interop.dll`, not `EffectGraph.dll`: a build for one runtime (`-r win-x64`) copies the native library into the same folder as the assemblies, and the two would overwrite each other. The bindings declare the COM interfaces of `EffectGraph.h` in vtable order with `[ComImport]`, the C entry points with `[DllImport("EffectGraph")]`, and pass Direct3D and Direct2D objects as `IntPtr`, so they work with any D3D wrapper or none. Methods returning `HRESULT` throw `COMException` on failure, except where `S_FALSE` carries meaning (`GetValuesBuffer`). Built-ins cannot come from C++ headers, so the generator also writes each as an `.effectblob`, the assembly embeds them, and `BuiltInEffects.Register(name)` hands one to `EffectGraphRegisterEffect`.

## License

Both packages are MIT-0 (`LICENSE-EffectGraph`), set as the nuspec's `license` expression and the C# project's `PackageLicenseExpression`. They contain no third-party source; the binaries compile in C++/WinRT headers (MIT, Microsoft). ExprTk and miniz are ShaderLab's alone (decision #224), so EffectGraph has no Numeric Expression evaluator and no `.effectgraph` loading: build graphs in code.

## Not yet

- Rust bindings are planned; they would ship as a crate rather than through NuGet.
- The packages are not signed.
