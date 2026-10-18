#pragma once

#include "pch_engine.h"

namespace ShaderLab::Graph
{
    struct EffectNode;

    // Where a Source node's image comes from. Saved graphs record it as a
    // true bool marker property (IsVideo, or a live source's marker), a media
    // path, or the Flood CLSID; ClassifySource reads them.
    enum class SourceKind : uint8_t
    {
        None,    // not a Source node, or nothing to produce yet
        Image,   // a still image file at mediaPath
        Flood,   // a D2D Flood of its Color property
        Video,   // a video file at mediaPath
        Live,    // frames from a host-registered live source (Effects/LiveSource.h)
    };

    inline constexpr const wchar_t* cVideoMarker = L"IsVideo";

    // Adds the marker property of a live source kind. Effects::RegisterLiveSourceKind
    // calls it; registering a marker twice is a no-op.
    void RegisterLiveSourceMarker(std::wstring_view marker);

    // The registered live-source marker `node` carries as a true bool, or
    // empty. A marker no host registered leaves the node None.
    std::wstring LiveSourceMarker(const EffectNode& node);

    // The kind of a node. A marker counts only when it is a true bool. An
    // Image node with an empty path is still an Image: it has no file yet.
    SourceKind ClassifySource(const EffectNode& node);

    inline bool IsLiveSource(SourceKind kind)
    {
        return kind == SourceKind::Live;
    }
}
