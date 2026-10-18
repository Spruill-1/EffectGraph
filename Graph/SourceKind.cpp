#include "pch_engine.h"
#include "SourceKind.h"
#include "EffectNode.h"

#include <shared_mutex>

namespace ShaderLab::Graph
{
    namespace
    {
        bool HasTrueMarker(const EffectNode& node, const std::wstring& marker)
        {
            auto it = node.properties.find(marker);
            if (it == node.properties.end()) return false;
            const auto* value = std::get_if<bool>(&it->second);
            return value && *value;
        }

        std::shared_mutex& MarkerMutex()
        {
            static std::shared_mutex mutex;
            return mutex;
        }

        std::vector<std::wstring>& Markers()
        {
            static std::vector<std::wstring> markers;
            return markers;
        }
    }

    void RegisterLiveSourceMarker(std::wstring_view marker)
    {
        if (marker.empty()) return;
        std::unique_lock lock(MarkerMutex());
        auto& markers = Markers();
        if (std::find(markers.begin(), markers.end(), marker) == markers.end())
            markers.emplace_back(marker);
    }

    std::wstring LiveSourceMarker(const EffectNode& node)
    {
        std::shared_lock lock(MarkerMutex());
        for (const auto& marker : Markers())
            if (HasTrueMarker(node, marker))
                return marker;
        return {};
    }

    SourceKind ClassifySource(const EffectNode& node)
    {
        if (node.type != NodeType::Source) return SourceKind::None;
        if (!LiveSourceMarker(node).empty())  return SourceKind::Live;
        static const std::wstring video = cVideoMarker;
        if (HasTrueMarker(node, video))        return SourceKind::Video;
        if (node.mediaPath.has_value() && !node.effectClsid.has_value()) return SourceKind::Image;
        if (node.effectClsid.has_value() && IsEqualGUID(*node.effectClsid, CLSID_D2D1Flood))
            return SourceKind::Flood;
        return SourceKind::None;
    }
}
