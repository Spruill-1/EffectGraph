#include "pch_engine.h"
#include "PropertyWrite.h"
#include "ClockModel.h"

#include <cwctype>

namespace ShaderLab::Graph
{
    namespace
    {
        namespace Numerics = winrt::Windows::Foundation::Numerics;

        enum class Target { Unknown, Float, Int, Uint, Bool, String, Float2, Float3, Float4, FloatArray, Other };

        Target TargetOfTypeName(std::wstring_view typeName)
        {
            if (typeName == L"float")  return Target::Float;
            if (typeName == L"int")    return Target::Int;
            if (typeName == L"uint")   return Target::Uint;
            if (typeName == L"bool")   return Target::Bool;
            if (typeName == L"float2") return Target::Float2;
            if (typeName == L"float3") return Target::Float3;
            if (typeName == L"float4") return Target::Float4;
            return Target::Unknown;
        }

        Target TargetOfValue(const PropertyValue& value)
        {
            return std::visit([](auto&& v) -> Target
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)                  return Target::Float;
                else if constexpr (std::is_same_v<T, int32_t>)           return Target::Int;
                else if constexpr (std::is_same_v<T, uint32_t>)          return Target::Uint;
                else if constexpr (std::is_same_v<T, bool>)              return Target::Bool;
                else if constexpr (std::is_same_v<T, std::wstring>)      return Target::String;
                else if constexpr (std::is_same_v<T, Numerics::float2>)  return Target::Float2;
                else if constexpr (std::is_same_v<T, Numerics::float3>)  return Target::Float3;
                else if constexpr (std::is_same_v<T, Numerics::float4>)  return Target::Float4;
                else if constexpr (std::is_same_v<T, std::vector<float>>) return Target::FloatArray;
                else                                                     return Target::Other;
            }, value);
        }

        bool IsPlayingKey(const std::wstring& key) { return key == L"IsPlaying" || key == L"isPlaying"; }
        bool IsMediaPathKey(const std::wstring& key) { return key == L"shaderPath" || key == L"mediaPath"; }

        // The declared parameter's type, else the current value's type.
        Target TargetFor(const EffectNode& node, const std::wstring& key)
        {
            if (node.customEffect.has_value())
            {
                for (const auto& param : node.customEffect->parameters)
                {
                    if (param.name != key) continue;
                    if (const Target declared = TargetOfTypeName(param.typeName); declared != Target::Unknown)
                        return declared;
                    break;
                }
            }
            if (auto existing = node.properties.find(key); existing != node.properties.end())
                return TargetOfValue(existing->second);
            return IsPlayingKey(key) ? Target::Bool : Target::Unknown;
        }

        std::wstring_view Trim(std::wstring_view text)
        {
            while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
            while (!text.empty() && std::iswspace(text.back()))  text.remove_suffix(1);
            return text;
        }

        // "[1, 2, 3]" or "1, 2, 3" as numbers, or nullopt.
        std::optional<std::vector<double>> ParseNumberList(std::wstring_view text)
        {
            text = Trim(text);
            if (text.size() >= 2 && text.front() == L'[' && text.back() == L']')
                text = text.substr(1, text.size() - 2);
            std::vector<double> numbers;
            while (true)
            {
                const size_t comma = text.find(L',');
                auto number = ParseNumber(text.substr(0, comma));
                if (!number) return std::nullopt;
                numbers.push_back(*number);
                if (comma == std::wstring_view::npos) break;
                text.remove_prefix(comma + 1);
            }
            return numbers;
        }

        std::wstring Quoted(const std::wstring& key) { return L"'" + key + L"'"; }

        std::wstring FromNumber(double number, Target target, const std::wstring& key, PropertyValue& out)
        {
            if (!std::isfinite(number)) return Quoted(key) + L" must be a finite number";
            switch (target)
            {
            case Target::Unknown:
            case Target::Float:
                if (!std::isfinite(static_cast<float>(number))) return Quoted(key) + L" is out of float range";
                out = static_cast<float>(number);
                return {};
            case Target::Uint:
                if (number < 0.0 || number > static_cast<double>(UINT32_MAX))
                    return Quoted(key) + L" expects a non-negative integer";
                out = static_cast<uint32_t>(number);
                return {};
            case Target::Int:
                if (number < static_cast<double>(INT32_MIN) || number > static_cast<double>(INT32_MAX))
                    return Quoted(key) + L" is out of int range";
                out = static_cast<int32_t>(number);
                return {};
            case Target::Bool:
                out = number != 0.0;
                return {};
            default:
                return Quoted(key) + L" cannot be set from a single number";
            }
        }

        std::wstring FromNumbers(const std::vector<double>& numbers, Target target, const std::wstring& key, PropertyValue& out)
        {
            for (double number : numbers)
                if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
                    return Quoted(key) + L" must hold finite float values";
            auto at = [&](size_t i) { return static_cast<float>(numbers[i]); };
            size_t wanted = 0;
            switch (target)
            {
            case Target::Float2: wanted = 2; break;
            case Target::Float3: wanted = 3; break;
            case Target::Float4: wanted = 4; break;
            case Target::FloatArray:
            {
                std::vector<float> values;
                for (size_t i = 0; i < numbers.size(); ++i) values.push_back(at(i));
                out = std::move(values);
                return {};
            }
            case Target::Unknown:
                if (numbers.size() >= 2 && numbers.size() <= 4) { wanted = numbers.size(); break; }
                return Quoted(key) + L" takes an array of 2, 3 or 4 numbers";
            default:
                return Quoted(key) + L" cannot be set from an array";
            }
            if (numbers.size() != wanted)
                return Quoted(key) + std::format(L" expects an array of {} numbers, got {}", wanted, numbers.size());
            if (wanted == 2)      out = Numerics::float2{ at(0), at(1) };
            else if (wanted == 3) out = Numerics::float3{ at(0), at(1), at(2) };
            else                  out = Numerics::float4{ at(0), at(1), at(2), at(3) };
            return {};
        }

        std::wstring FromString(const std::wstring& text, Target target, const std::wstring& key, PropertyValue& out)
        {
            switch (target)
            {
            case Target::Unknown:
            case Target::String:
                out = text;
                return {};
            case Target::Bool:
                if (auto flag = ParseBool(text)) { out = *flag; return {}; }
                return L"expected a boolean-valued string (true/false, 1/0, on/off, yes/no)";
            case Target::Float:
            case Target::Int:
            case Target::Uint:
                if (auto number = ParseNumber(text)) return FromNumber(*number, target, key, out);
                return Quoted(key) + L" expects a number, got '" + text + L"'";
            case Target::Float2:
            case Target::Float3:
            case Target::Float4:
            case Target::FloatArray:
                if (auto numbers = ParseNumberList(text)) return FromNumbers(*numbers, target, key, out);
                return Quoted(key) + L" expects an array of numbers, got '" + text + L"'";
            default:
                return Quoted(key) + L" cannot be set from a string";
            }
        }

        std::optional<bool> AsBool(const PropertyValue& value)
        {
            if (auto* flag = std::get_if<bool>(&value))      return *flag;
            if (auto* f = std::get_if<float>(&value))        return *f != 0.0f;
            if (auto* i = std::get_if<int32_t>(&value))      return *i != 0;
            if (auto* u = std::get_if<uint32_t>(&value))     return *u != 0u;
            if (auto* s = std::get_if<std::wstring>(&value)) return ParseBool(*s);
            return std::nullopt;
        }
    }

    std::optional<double> ParseNumber(std::wstring_view text)
    {
        const std::wstring trimmed(Trim(text));
        if (trimmed.empty()) return std::nullopt;
        wchar_t* end = nullptr;
        const double value = std::wcstod(trimmed.c_str(), &end);
        if (end != trimmed.c_str() + trimmed.size() || !std::isfinite(value)) return std::nullopt;
        return value;
    }

    std::optional<bool> ParseBool(std::wstring_view text)
    {
        std::wstring lower;
        for (wchar_t ch : Trim(text)) lower += static_cast<wchar_t>(std::towlower(ch));
        if (lower == L"true"  || lower == L"1" || lower == L"on"  || lower == L"yes") return true;
        if (lower == L"false" || lower == L"0" || lower == L"off" || lower == L"no")  return false;
        return std::nullopt;
    }

    std::wstring WriteProperty(EffectNode& node, const std::wstring& key, const UntypedValue& value)
    {
        const Target target = TargetFor(node, key);
        PropertyValue converted;
        std::wstring error = std::visit([&](auto&& v) -> std::wstring
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return FromNumber(v, target, key, converted);
            else if constexpr (std::is_same_v<T, bool>)
            {
                switch (target)
                {
                case Target::Float: converted = v ? 1.0f : 0.0f; return {};
                case Target::Int:   converted = static_cast<int32_t>(v ? 1 : 0); return {};
                case Target::Uint:  converted = v ? 1u : 0u; return {};
                case Target::Unknown:
                case Target::Bool:  converted = v; return {};
                default:            return Quoted(key) + L" cannot be set from a boolean";
                }
            }
            else if constexpr (std::is_same_v<T, std::wstring>)
                return FromString(v, target, key, converted);
            else
                return FromNumbers(v, target, key, converted);
        }, value);
        if (!error.empty()) return error;

        std::optional<bool> play;
        if (IsPlayingKey(key))
        {
            play = AsBool(converted);
            if (!play) return L"isPlaying must be a boolean, number or true/false string";
        }

        // A Source's file lives in mediaPath, not in properties.
        if (IsMediaPathKey(key))
        {
            const auto* path = std::get_if<std::wstring>(&converted);
            if (!path) return Quoted(key) + L" must be a file path string";
            node.mediaPath = *path;
            node.dirty = true;
            return {};
        }

        node.properties[key] = converted;
        if (play)
        {
            if (node.IsClock()) SetClockPlaying(node, *play);
            else                node.isPlaying = *play;
        }
        node.dirty = true;
        return {};
    }
}
