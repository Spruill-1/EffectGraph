#pragma once

// EffectGraph plugins: a DLL that adds effects to whichever EffectGraph loads it.
//
// A plugin exports one function, EffectGraphPluginRegister, which the host
// calls once with an IEffectGraphPluginHost. The plugin registers its effects
// through that interface, never through the EffectGraph* C functions: those
// would load EffectGraph.dll into the process and register into its engine,
// not the host's. So a plugin does not link EffectGraph.lib, and includes
// generated effect headers with EFFECTGRAPH_NO_STARTUP_REGISTRATION defined:
//
//     #define EFFECTGRAPH_NO_STARTUP_REGISTRATION
//     #include "EffectGraphPlugin.h"
//     #include "MyToneMap.effect.h"          // EffectGraphCompile output
//
//     EFFECTGRAPH_PLUGIN_EXPORT HRESULT WINAPI EffectGraphPluginRegister(IEffectGraphPluginHost* host)
//     {
//         return host->RegisterEffects(&cMyToneMapEffect);
//     }
//
// A host loads a plugin with EffectGraphLoadPlugin and never unloads it, so
// the plugin's effect arrays stay valid for the life of the process. New host
// capabilities arrive as new interfaces a plugin queries the host for; this
// one does not change.

#include "EffectGraph.h"

// Registers effects into the host's engine. Results are those of
// EffectGraphRegisterEffects and EffectGraphRegisterEffect.
MIDL_INTERFACE("6f4a1c92-5b3e-4d71-a8c6-2e9d0b7f3a15")
IEffectGraphPluginHost : public IUnknown
{
    // The effects of a generated header, without copying: `source` and the
    // arrays it points at must stay valid, as they do in a loaded plugin.
    STDMETHOD(RegisterEffects)(const EFFECTGRAPH_EFFECT_SOURCE* source) PURE;

    // The effects of a blob file written by EffectGraphCompile --out; copied.
    STDMETHOD(RegisterEffect)(const void* data, UINT32 size) PURE;
};

#define EFFECTGRAPH_PLUGIN_ENTRY_NAME "EffectGraphPluginRegister"
#define EFFECTGRAPH_PLUGIN_EXPORT extern "C" __declspec(dllexport)

typedef HRESULT(WINAPI* PFN_EFFECTGRAPH_PLUGIN_REGISTER)(IEffectGraphPluginHost* host);

// Loads the plugin at `path` (relative to the current directory unless full)
// and calls its EffectGraphPluginRegister with this engine's host. The plugin
// stays loaded until the process ends. Nothing loads a plugin but this call.
// Returns what the plugin returned; S_FALSE without calling it again when it is
// already loaded; HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) when the DLL is not
// a plugin (it is unloaded again); the loader's error when it does not load.
// Its dependencies are found next to it first. Call at startup, before
// creating nodes, as for EffectGraphRegisterEffects.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphLoadPlugin(PCWSTR path);
