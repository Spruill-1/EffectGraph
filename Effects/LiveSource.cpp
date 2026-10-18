#include "pch_engine.h"
#include "LiveSource.h"
#include "../Graph/SourceKind.h"

#include <shared_mutex>

namespace ShaderLab::Effects
{
    namespace
    {
        struct Registry
        {
            std::shared_mutex mutex;
            std::unordered_map<std::wstring, std::shared_ptr<const LiveSourceKind>> kinds;
        };

        Registry& Kinds()
        {
            static Registry registry;
            return registry;
        }
    }

    void RegisterLiveSourceKind(LiveSourceKind kind)
    {
        if (kind.marker.empty() || !kind.create) return;
        Graph::RegisterLiveSourceMarker(kind.marker);
        auto& registry = Kinds();
        std::unique_lock lock(registry.mutex);
        auto marker = kind.marker;
        registry.kinds[std::move(marker)] = std::make_shared<const LiveSourceKind>(std::move(kind));
    }

    std::shared_ptr<const LiveSourceKind> FindLiveSourceKind(const Graph::EffectNode& node)
    {
        const std::wstring marker = Graph::LiveSourceMarker(node);
        if (marker.empty()) return nullptr;
        auto& registry = Kinds();
        std::shared_lock lock(registry.mutex);
        auto it = registry.kinds.find(marker);
        return it == registry.kinds.end() ? nullptr : it->second;
    }
}
