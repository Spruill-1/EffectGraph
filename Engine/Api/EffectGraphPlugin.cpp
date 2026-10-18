#include "pch_engine.h"
#include "EffectGraphPlugin.h"

namespace
{
    // The engine's side of the plugin contract: the C registration functions,
    // reached through an interface so the plugin needs no import of them.
    struct PluginHost : winrt::implements<PluginHost, IEffectGraphPluginHost>
    {
        STDMETHODIMP RegisterEffects(const EFFECTGRAPH_EFFECT_SOURCE* source) override
        {
            return EffectGraphRegisterEffects(source);
        }

        STDMETHODIMP RegisterEffect(const void* data, UINT32 size) override
        {
            return EffectGraphRegisterEffect(data, size);
        }
    };

    std::mutex sPluginMutex;
    std::vector<HMODULE> sPlugins;
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphLoadPlugin(PCWSTR path)
{
    if (!path || !*path) return E_INVALIDARG;
    try
    {
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR takes only a fully qualified path.
        const std::wstring fullPath = std::filesystem::absolute(path).wstring();
        std::lock_guard lock(sPluginMutex);
        // The plugin's own folder first for its dependencies, then the system's.
        HMODULE module = ::LoadLibraryExW(fullPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return HRESULT_FROM_WIN32(::GetLastError());
        if (std::find(sPlugins.begin(), sPlugins.end(), module) != sPlugins.end())
        {
            ::FreeLibrary(module);   // drops the reference this call added
            return S_FALSE;
        }
        const auto entry = reinterpret_cast<PFN_EFFECTGRAPH_PLUGIN_REGISTER>(
            ::GetProcAddress(module, EFFECTGRAPH_PLUGIN_ENTRY_NAME));
        if (!entry)
        {
            ::FreeLibrary(module);
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        // Kept loaded even when the plugin fails: it may have registered some
        // effects whose arrays live in it.
        sPlugins.push_back(module);
        const auto host = winrt::make<PluginHost>();
        return entry(host.as<IEffectGraphPluginHost>().get());
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}
