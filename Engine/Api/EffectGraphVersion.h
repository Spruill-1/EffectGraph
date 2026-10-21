#pragma once

// EffectGraph's version, independent of any program built on it. The NuGet
// packages take theirs from EFFECTGRAPH_VERSION_STRING (packaging/Pack.ps1).
//   MAJOR: an incompatible change to the C API, an interface or the effect blob
//   MINOR: additions (functions, interfaces, built-in effects)
//   PATCH: fixes
// A shipped interface is never changed in place: a new one gets a new IID.

#define EFFECTGRAPH_VERSION_MAJOR 1
#define EFFECTGRAPH_VERSION_MINOR 0
#define EFFECTGRAPH_VERSION_PATCH 1
#define EFFECTGRAPH_VERSION_STRING "1.0.1"
