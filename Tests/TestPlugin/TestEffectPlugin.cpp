// An EffectGraph plugin, built the way a real one is: an effect compiled by
// EffectGraphCompile into a header, registered through the host's interface.
// It links nothing from EffectGraph. The tests load it with EffectGraphLoadPlugin.
#define EFFECTGRAPH_NO_STARTUP_REGISTRATION
#include <windows.h>
#include "EffectGraphPlugin.h"
#include "TestPluginGain.effect.h"

EFFECTGRAPH_PLUGIN_EXPORT HRESULT WINAPI EffectGraphPluginRegister(IEffectGraphPluginHost* host)
{
    if (!host) return E_INVALIDARG;
    return host->RegisterEffects(&cTestPluginGainEffect);
}
