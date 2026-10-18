# EffectGraph.Engine

EffectGraph's engine as a static C++ library with all of its headers: the graph
model (`Graph/`), the evaluator and frame order (`Rendering/`), the effect
registry, compiler and sources (`Effects/`), and the public and internal API
(`Engine/Api/`). ShaderLab is built on it.

**Most programs want `EffectGraph.Native`**, the DLL with a stable COM API. This
package is for a program that works inside the engine. Its headers are not a
stable API; they change with every version, so pin the version you build against.

- x64 and ARM64, Debug and Release (chosen by the project's `UseDebugLibraries`).
- Built with MSVC v143 (Visual Studio 2022; it links with that and any newer toolset), C++20, without `/GL`, debug information in the objects.
- Needs C++/WinRT (`Microsoft.Windows.CppWinRT`) in the consuming project.
- `build/native/EffectGraph.Engine.targets` adds the include path, defines
  `EFFECTGRAPH_STATIC` and links the library. Include engine headers from the
  include root, `#include "Graph/GraphModel.h"`; the public API and the built-in
  effect headers by bare name, `#include "EffectGraphBuiltIns.h"`.
- A precompiled header: compile one `.cpp` containing `#include "pch_engine.h"`
  with `/Yc`, and use it in the rest.

License: MIT-0.
