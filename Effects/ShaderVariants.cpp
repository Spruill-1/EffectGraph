#include "pch_engine.h"
#include "ShaderVariants.h"
#include "BytecodeCache.h"
#include "../Engine/Utf8.h"

#include <cmath>

namespace ShaderLab::Effects
{
    namespace
    {
        struct Option
        {
            std::string  name;      // UTF-8
            std::wstring wideName;
            uint32_t     valueCount;
        };

        std::vector<Option> SpecializedOptions(const Graph::CustomEffectDefinition& def)
        {
            std::vector<Option> options;
            for (const auto& param : def.parameters)
            {
                if (!param.specialize || param.gpuBindable || param.enumLabels.empty()) continue;
                if (options.size() >= cMaxSpecializedOptions) break;
                Option option;
                option.wideName = param.name;
                option.name = WideToUtf8(param.name);
                option.valueCount = static_cast<uint32_t>((std::min<size_t>)(param.enumLabels.size(), cMaxOptionValues));
                options.push_back(std::move(option));
            }
            return options;
        }

        // The option index a property value names, or -1 when it names none.
        int OptionIndex(const Graph::PropertyValue& value, uint32_t valueCount)
        {
            double number = 0.0;
            if (auto* floatValue = std::get_if<float>(&value))         number = *floatValue;
            else if (auto* uintValue = std::get_if<uint32_t>(&value))  number = *uintValue;
            else if (auto* intValue = std::get_if<int32_t>(&value))    number = *intValue;
            else if (auto* boolValue = std::get_if<bool>(&value))      number = *boolValue ? 1.0 : 0.0;
            else return -1;
            const double rounded = std::round(number);
            // Only an exact option picks a variant: the shader's own cast would
            // truncate a fractional value differently. A value just below an
            // option truncates to the one before it in the generic build, so it
            // goes there too.
            if (!std::isfinite(number) || std::abs(number - rounded) > 1e-4 || number < rounded ||
                rounded < 0.0 || rounded >= valueCount) return -1;
            return static_cast<int>(rounded);
        }
    }

    std::vector<std::string> SpecializedOptionNames(const Graph::CustomEffectDefinition& def)
    {
        std::vector<std::string> names;
        for (auto& option : SpecializedOptions(def)) names.push_back(std::move(option.name));
        return names;
    }

    uint64_t OptionKeyFor(const Graph::CustomEffectDefinition& def,
                          const std::map<std::wstring, Graph::PropertyValue>& values)
    {
        const auto options = SpecializedOptions(def);
        uint64_t key = 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
            auto it = values.find(options[i].wideName);
            if (it == values.end()) return 0;              // no value: generic
            const int optionIndex = OptionIndex(it->second, options[i].valueCount);
            if (optionIndex < 0) return 0;                 // not an option: generic
            key |= static_cast<uint64_t>(optionIndex + 1) << (8u * i);
        }
        return key;
    }

    size_t OptionCombinationCount(const Graph::CustomEffectDefinition& def)
    {
        const auto options = SpecializedOptions(def);
        if (options.empty()) return 0;
        size_t combinations = 1;
        for (const auto& option : options)
        {
            combinations *= option.valueCount;
            if (combinations > 1'000'000) break;
        }
        return combinations;
    }

    std::vector<uint64_t> AllOptionKeys(const Graph::CustomEffectDefinition& def)
    {
        const auto options = SpecializedOptions(def);
        std::vector<uint64_t> keys;
        const size_t total = OptionCombinationCount(def);
        if (total == 0 || total > cMaxOptionCombinations) return keys;
        keys.reserve(total);
        // Count through every combination like an odometer, one digit per option.
        std::vector<uint32_t> digits(options.size(), 0);
        for (size_t combination = 0; combination < total; ++combination)
        {
            uint64_t key = 0;
            for (size_t i = 0; i < options.size(); ++i)
                key |= static_cast<uint64_t>(digits[i] + 1) << (8u * i);
            keys.push_back(key);
            for (size_t i = 0; i < options.size(); ++i)
            {
                if (++digits[i] < options[i].valueCount) break;
                digits[i] = 0;
            }
        }
        return keys;
    }

    uint32_t PackBindingModes(std::span<const GpuBindingMode> modes)
    {
        uint32_t bits = 0;
        for (size_t index = 0; index < modes.size(); ++index)
            bits = WithBindingMode(bits, index, modes[index]);
        return bits;
    }

    std::vector<std::string> GpuBindableNames(const Graph::CustomEffectDefinition& def)
    {
        std::vector<std::string> names;
        for (const auto& param : def.parameters)
            if (param.gpuBindable) names.push_back(WideToUtf8(param.name));
        return names;
    }

    std::vector<MacroPair> BuildVariantMacros(
        const std::vector<std::string>& gpuNames, uint32_t bindingBits,
        const std::vector<std::string>& optionNames, uint64_t optionKey)
    {
        std::vector<MacroPair> macros;
        macros.reserve(gpuNames.size() + 2 * optionNames.size());
        for (size_t index = 0; index < gpuNames.size(); ++index)
        {
            const auto mode = static_cast<uint32_t>(BindingModeAt(bindingBits, index));
            macros.push_back({ "_SLPARAM_" + gpuNames[index] + "_GPU", std::to_string(mode) });
        }
        for (size_t index = 0; index < optionNames.size() && index < cMaxSpecializedOptions; ++index)
        {
            const uint32_t slot = OptionKeySlot(optionKey, index);
            macros.push_back({ "_SLOPT_" + optionNames[index] + "_MODE", slot ? "1" : "0" });
            macros.push_back({ "_SLOPT_" + optionNames[index], std::to_string(slot ? slot - 1u : 0u) });
        }
        return macros;
    }

    std::vector<MacroPair> AnalysisFieldMacros(const Graph::CustomEffectDefinition& def)
    {
        std::vector<MacroPair> macros;
        std::set<std::string> taken;
        uint32_t pixelOffset = 0;
        for (const auto& field : def.analysisFields)
        {
            std::string name = "SL_FIELD_";
            for (wchar_t ch : field.name)
                name.push_back(ch < 128 && (std::isalnum(static_cast<int>(ch)) || ch == L'_') ? static_cast<char>(ch) : '_');
            if (taken.insert(name).second)
                macros.push_back({ std::move(name), std::to_string(pixelOffset) });
            pixelOffset += field.pixelCount();
        }
        return macros;
    }

    std::vector<MacroPair> GenericVariantMacros(const Graph::CustomEffectDefinition& def)
    {
        auto macros = BuildVariantMacros(GpuBindableNames(def), 0, SpecializedOptionNames(def), 0);
        const auto fields = AnalysisFieldMacros(def);
        macros.insert(macros.end(), fields.begin(), fields.end());
        return macros;
    }

    BytecodeCompileRequest MakeVariantRequest(
        const Graph::CustomEffectDefinition& def, std::string canonicalHlsl,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target,
        uint32_t bindingBits, uint64_t optionKey)
    {
        BytecodeCompileRequest request;
        request.gpuBindableParamNames = GpuBindableNames(def);
        // Option names go with every compile, the baseline included:
        // SHADERLAB_OPTION needs _SLOPT_<name>_MODE defined even for the generic build.
        request.optionParamNames      = SpecializedOptionNames(def);
        request.key.sourceHash         = HashCanonicalSource(canonicalHlsl);
        request.key.paramSignatureHash = HashParamSignature(request.gpuBindableParamNames, request.optionParamNames);
        request.fieldMacros            = AnalysisFieldMacros(def);
        if (!request.fieldMacros.empty())
        {
            // The field layout is part of what was compiled.
            std::string layout(reinterpret_cast<const char*>(&request.key.paramSignatureHash), sizeof(uint64_t));
            for (const auto& [name, value] : request.fieldMacros)
            {
                layout.append(name); layout.push_back('=');
                layout.append(value); layout.push_back('\0');
            }
            request.key.paramSignatureHash = FNV1a64(layout.data(), layout.size());
        }
        request.key.includeLibraryHash = IncludeLibraryHash();
        request.key.macroBitset        = bindingBits;
        request.key.optionKey          = optionKey;
        request.key.entryPoint         = "main";
        request.key.target             = target;
        request.metadata.effectId      = effectId;
        request.metadata.version       = effectVersion;
        request.hlslSource             = std::move(canonicalHlsl);
        return request;
    }

    BytecodeCompileRequest MakeVariantRequest(
        const Graph::CustomEffectDefinition& def,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target,
        uint32_t bindingBits, uint64_t optionKey)
    {
        return MakeVariantRequest(def, CanonicalizeHlslSource(def.hlslSource),
            effectId, effectVersion, target, bindingBits, optionKey);
    }

    CompileIdentity CompileIdentityOf(const Graph::EffectNode& node)
    {
        if (node.customEffect.has_value() && !node.customEffect->shaderLabEffectId.empty())
            return { node.customEffect->shaderLabEffectId,
                     node.customEffect->shaderLabEffectVersion ? node.customEffect->shaderLabEffectVersion : 1u };
        return { node.name, 1u };
    }

    std::string ShaderTargetFor(const Graph::CustomEffectDefinition& def)
    {
        return def.shaderType == Graph::CustomShaderType::PixelShader ? "ps_5_0" : "cs_5_0";
    }

    void PrecompileBindingShapes(
        BytecodeCache& cache, const Graph::CustomEffectDefinition& def,
        const std::wstring& effectId, uint32_t effectVersion, const std::string& target)
    {
        if (!SpecializedOptionNames(def).empty()) return;
        const size_t paramCount = (std::min)(GpuBindableNames(def).size(), cMaxGpuBindableParams);
        if (paramCount == 0) return;
        const std::string canonical = CanonicalizeHlslSource(def.hlslSource);
        const GpuBindingMode gpuMode = target.starts_with("ps")
            ? GpuBindingMode::Texture : GpuBindingMode::StructuredBuffer;
        cache.RequestCompile(MakeVariantRequest(def, canonical, effectId, effectVersion, target, 0, 0));
        for (size_t index = 0; index < paramCount; ++index)
            cache.RequestCompile(MakeVariantRequest(def, canonical, effectId, effectVersion, target,
                WithBindingMode(0, index, gpuMode), 0));
    }
}
