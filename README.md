# EffectGraph

A Direct2D effect graph engine for D3D11 programs, built for HDR and wide-gamut
work: the pipeline is scRGB FP16 linear light throughout, custom pixel and compute
shader effects are first-class nodes, and analysis effects (statistics, scopes,
ΔE ITP) report numbers a program can read on the CPU or bind on the GPU.

- **For any D3D11 program:** `EffectGraph.dll`, a C API of seven functions and a set
  of COM interfaces (`IEffectGraphContext`, `IEffectGraphNode`, …). Feed it textures,
  realize a node into your own texture, read its values.
- **Effects as data:** an effect is a blob compiled at build time from an Effect
  Designer `.json` (`EffectGraphCompile`, wired up by `build/EffectGraph.targets`).
  Every built-in comes as a header you include; a DLL can add effects to any host as
  a plugin (`EffectGraphPlugin.h`).
- **For .NET:** C# bindings over the same API.

[ShaderLab](https://github.com/Spruill-1/ShaderLab), the authoring and debugging
tool these effects come from, is built on EffectGraph.

## Packages

| Package | For |
|---|---|
| `EffectGraph.Native` | C++: the DLL, import libraries, headers, every built-in as a header and a blob, the effect compiler and MSBuild targets |
| `EffectGraph` | .NET: C# bindings with every built-in embedded |
| `EffectGraph.Engine` | Programs working inside the engine (ShaderLab): the static library and its internal headers; not a stable API |

## Build

Windows, Visual Studio 2022 or later with the C++ desktop workload; x64 and ARM64.

```pwsh
nuget restore EffectGraph.slnx -SolutionDirectory .
$vs  = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
& "$vs\MSBuild\Current\Bin\MSBuild.exe" EffectGraph.slnx /p:Configuration=Debug /p:Platform=x64 /m
.\x64\Debug\EffectGraphTests\EffectGraphTests.exe --adapter warp
pwsh -File packaging\Pack.ps1     # the three packages, into packaging\out
```

On an ARM64 machine use `MSBuild\Current\Bin\arm64\MSBuild.exe`; see
[docs/development/build.md](docs/development/build.md).

## Documentation

[docs/](docs/README.md): the API, the packages, the graph model and evaluation, and
the built-in effects.

## License

MIT No Attribution ([LICENSE](LICENSE)).
