#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"

namespace ShaderLab::Effects { class SourceNodeFactory; }

// How a host drives Source nodes between frames: which ones to prepare, how a
// failure is recorded, and how an offline render waits for video frames.
namespace ShaderLab::Rendering
{
    class GraphEvaluator;

    struct SourceDevices
    {
        ID2D1DeviceContext5* dc{ nullptr };
        ID3D11Device* d3dDevice{ nullptr };
        ID3D11DeviceContext* d3dContext{ nullptr };
    };

    enum class SourceScope
    {
        // Every Source node: after a load, or a one-shot render.
        All,
        // Source nodes that are dirty or play a video: every live frame.
        Changed,
    };

    // Runs SourceNodeFactory::PrepareSourceNode over the Source nodes in
    // scope. A failure lands on the node's runtimeError and does not stop
    // the other sources; one that throws also leaves the node clean, so it
    // does not keep the graph re-rendering.
    void PrepareSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory,
                                      const SourceDevices& devices, SourceScope scope);

    // Ticks video sources until each shows the frame for its Time, for hosts
    // with no render loop to do it over time. A Time driven by a binding is
    // resolved first with a frozen Evaluate, with every dirty flag put back.
    // A video that misses the timeout prints a [ShaderLab] line to stderr and
    // is not waited on again at that time until the factory forgets its
    // timeouts (graph load or clear). Returns false if any video missed.
    bool SettleVideoSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory,
                                          GraphEvaluator& evaluator, ID2D1DeviceContext5* dc,
                                          std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
