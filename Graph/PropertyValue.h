#pragma once

#include "pch_engine.h"

namespace ShaderLab::Graph
{
    // Variant type for node property values.
    // Covers common D2D effect property types: scalars, vectors, colors, strings,
    // enums, 5×4 color matrices, and variable-length float arrays (LUTs).
    using PropertyValue = std::variant<
        float,
        int32_t,
        uint32_t,
        bool,
        std::wstring,
        winrt::Windows::Foundation::Numerics::float2,
        winrt::Windows::Foundation::Numerics::float3,
        winrt::Windows::Foundation::Numerics::float4,
        D2D1_MATRIX_5X4_F,
        std::vector<float>
    >;

    // Human-readable tag for property type (used in JSON and UI).
    inline std::wstring PropertyValueTypeTag(const PropertyValue& value)
    {
        return std::visit([](auto&& v) -> std::wstring
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>)      return L"float";
            else if constexpr (std::is_same_v<T, int32_t>)    return L"int";
            else if constexpr (std::is_same_v<T, uint32_t>)   return L"uint";
            else if constexpr (std::is_same_v<T, bool>)       return L"bool";
            else if constexpr (std::is_same_v<T, std::wstring>) return L"string";
            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>) return L"float2";
            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>) return L"float3";
            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>) return L"float4";
            else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>) return L"matrix5x4";
            else if constexpr (std::is_same_v<T, std::vector<float>>) return L"floatarray";
            else { static_assert(sizeof(T) == 0, "Unhandled PropertyValue type"); return L"unknown"; }
        }, value);
    }

    // How many binding components a property has: 1 to 4 for float to
    // float4, 0 for a float array (bound whole), 1 for any other scalar.
    inline uint32_t PropertyComponentCount(const PropertyValue& value)
    {
        return std::visit([](auto&& v) -> uint32_t
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>) return 2;
            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>) return 3;
            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>) return 4;
            else if constexpr (std::is_same_v<T, std::vector<float>>) return 0;
            else return 1;
        }, value);
    }
}
