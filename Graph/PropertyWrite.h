#pragma once

#include "pch_engine.h"
#include "EffectNode.h"

// Writing a property value that arrives untyped -- from MCP, a script, or any
// client that does not know the node's property types -- into a node.
namespace ShaderLab::Graph
{
    // A value as a client sends it: a number, a boolean, a string, or an
    // array of numbers. Clients may send any of them as a string ("203",
    // "true"); MCP clients send untyped arguments that way.
    using UntypedValue = std::variant<double, bool, std::wstring, std::vector<double>>;

    // Writes `value` to node.properties[key] as the property's type: the
    // declared parameter's typeName, else the type of the value it replaces.
    // A string is parsed as that type. A key with neither keeps the value's
    // own type (a number becomes float, a string stays a string).
    //
    // Returns an empty string on success, else why the value cannot be
    // stored; nothing is written then. `IsPlaying` / `isPlaying` also starts
    // or pauses a clock (SetClockPlaying). `mediaPath`, or its saved name
    // `shaderPath`, sets EffectNode::mediaPath instead of a property. The
    // node is marked dirty.
    std::wstring WriteProperty(EffectNode& node, const std::wstring& key, const UntypedValue& value);

    // What a string means as a boolean: true/false, 1/0, on/off, yes/no, in
    // any case. nullopt for anything else, so a typo is an error rather than
    // a silent false.
    std::optional<bool> ParseBool(std::wstring_view text);

    // The whole of `text`, surrounding spaces aside, as a finite number;
    // nullopt for anything else ("12abc", "", "nan").
    std::optional<double> ParseNumber(std::wstring_view text);
}
