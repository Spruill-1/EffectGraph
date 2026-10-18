#pragma once

#include "pch_engine.h"

namespace ShaderLab::Graph
{
    enum class NodeType : uint32_t
    {
        Source = 0,
        BuiltInEffect = 1,
        PixelShader = 2,
        ComputeShader = 3,
        Output = 4,
    };

    inline std::wstring NodeTypeToString(NodeType type)
    {
        switch (type)
        {
        case NodeType::Source:         return L"Source";
        case NodeType::BuiltInEffect:  return L"BuiltInEffect";
        case NodeType::PixelShader:    return L"PixelShader";
        case NodeType::ComputeShader:  return L"ComputeShader";
        case NodeType::Output:         return L"Output";
        default:                       return L"Unknown";
        }
    }

    inline NodeType NodeTypeFromString(std::wstring_view str)
    {
        if (str == L"Source")         return NodeType::Source;
        if (str == L"BuiltInEffect")  return NodeType::BuiltInEffect;
        if (str == L"PixelShader")    return NodeType::PixelShader;
        if (str == L"ComputeShader")  return NodeType::ComputeShader;
        if (str == L"Output")         return NodeType::Output;
        throw std::invalid_argument("Unknown NodeType string");
    }
}
