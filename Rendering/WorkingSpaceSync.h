#pragma once

// Working Space node sync.
//
// The "Working Space" parameter node mirrors the active display profile
// (live or simulated) into 14 typed analysis output fields exposed as
// node properties. Downstream effects can bind any property to one of
// these fields to track the active display profile automatically.
//
// A pure walk over the graph and one DisplayProfile value, so the GUI, the
// headless host and the display-profile MCP routes share it and it needs no
// device or DisplayMonitor.


namespace ShaderLab::Graph
{
    class GraphModel;
}

namespace ShaderLab::Rendering
{
    struct DisplayProfile;

    // Walk the graph and refresh every "Working Space" parameter node's
    // properties from `profile` (callers pass DisplayMonitor::ActiveProfile()).
    // Marks the node dirty only when at least one field changed value, so
    // callers can pump this every render tick without invalidating the cache
    // when the profile is stable. Returns true if any node was modified.
    bool UpdateWorkingSpaceNodes(
        Graph::GraphModel& graph,
        const DisplayProfile& profile);
}
