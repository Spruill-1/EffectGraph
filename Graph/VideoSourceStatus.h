#pragma once

#include <cstdint>

namespace ShaderLab::Graph
{
    // Playback state of one video source, copied from its provider on the
    // render thread so the UI never touches a live provider.
    struct VideoSourceStatus
    {
        std::uint32_t nodeId{ 0 };
        bool open{ false };
        bool loop{ false };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        double frameRate{ 0.0 };
        double duration{ 0.0 };
        double position{ 0.0 };
        std::uint64_t uploads{ 0 };
    };
}
