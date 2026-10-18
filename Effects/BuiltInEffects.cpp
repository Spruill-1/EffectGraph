#include "pch_engine.h"
#include "BuiltInEffects.h"
#include "../Graph/EffectNode.h"
#include "../Engine/Utf8.h"

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace ShaderLab::Effects
{
    namespace
    {
        constexpr BuiltInEffectInfo scBuiltInEffects[] = {
            { L"Luminance Heatmap", L"Luminance Heatmap", true },
            { L"Gamut Highlight", L"Gamut Highlight", true },
            { L"Luminance Highlight", L"Luminance Highlight", true },
            { L"CIE Histogram", L"CIE Histogram", true },
            { L"CIE Chromaticity Plot", L"CIE Chromaticity Plot", true },
            { L"Gamut Source", L"Gamut Source", true },
            { L"Color Checker", L"Color Checker", true },
            { L"Zone Plate", L"Zone Plate", true },
            { L"Gradient Generator", L"Gradient Generator", true },
            { L"HDR Test Pattern", L"HDR Test Pattern", true },
            { L"Delta E Comparator", L"Delta E Comparator", true },
            { L"Nit Map", L"Nit Map", true },
            { L"Gamut Coverage", L"Gamut Coverage", true },
            { L"Gamut Map", L"Gamut Map", true },
            { L"ICtCp Gamut Map", L"ICtCp Gamut Map", true },
            { L"ICtCp Boundary", L"ICtCp Boundary", true },
            { L"ICtCp Gamut Boundary LUT", L"ICtCp Gamut Boundary LUT", true },
            { L"Gamut LUT Viewer", L"Gamut LUT Viewer", true },
            { L"ICtCp Round-Trip Validator", L"ICtCp Round-Trip Validator", true },
            { L"ICtCp Tone Map (HDR -> SDR)", L"ICtCp Tone Map", true },
            { L"ICtCp Inverse Tone Map (SDR -> HDR)", L"ICtCp Inverse Tone Map", true },
            { L"ICtCp Saturation", L"ICtCp Saturation", true },
            { L"ICtCp Highlight Desaturation", L"ICtCp Highlight Desaturation", true },
            { L"Split Comparison", L"Split Comparison", true },
            { L"Side by Side", L"Side by Side", true },
            { L"Channel Statistics", L"Channel Statistics", true },
            { L"Luminance Statistics", L"Luminance Statistics", true },
            { L"Chromaticity Statistics", L"Chromaticity Statistics", true },
            { L"Float Parameter", L"Float Parameter", false },
            { L"Integer Parameter", L"Integer Parameter", false },
            { L"Toggle Parameter", L"Toggle Parameter", false },
            { L"Gamut Parameter", L"Gamut Parameter", false },
            { L"Clock", Graph::cClockEffectId, false },
            { L"Numeric Expression", Graph::cMathExpressionEffectId, false },
            { L"Random", Graph::cRandomEffectId, false },
            { L"Scale", L"ShaderLab Scale", true },
            { L"Image Info", L"Image Info", true },
            { L"Working Space", Graph::cWorkingSpaceEffectId, false },
        };

        // The namespace of every effect class id (RFC 9562 version 5).
        constexpr uint8_t scClsidNamespace[16] = {
            0x5d, 0x90, 0x61, 0x54, 0x19, 0xb3, 0x44, 0x41,
            0x95, 0x5a, 0x9c, 0x4f, 0xb9, 0xd3, 0xb5, 0xa9,
        };

        bool SameName(std::wstring_view a, std::wstring_view b)
        {
            return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                          b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        // The words of a name: runs of letters and digits.
        std::vector<std::string> Words(std::wstring_view name)
        {
            std::vector<std::string> words(1);
            for (wchar_t ch : name)
            {
                const bool letter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
                const bool digit = ch >= L'0' && ch <= L'9';
                if (!letter && !digit)
                {
                    if (!words.back().empty()) words.emplace_back();
                    continue;
                }
                words.back().push_back(static_cast<char>(ch));
            }
            if (words.back().empty()) words.pop_back();
            return words;
        }
    }

    Graph::ParameterDefinition GamutParameter(const wchar_t* name, float defaultId, const wchar_t* visibleWhen)
    {
        static const std::vector<std::wstring> sLabels{ L"sRGB", L"Display P3", L"BT.2020", L"Custom", L"DCI-P3" };
        return { name, L"float", defaultId, 0.0f, 4.0f, 1.0f, sLabels, visibleWhen };
    }

    std::span<const BuiltInEffectInfo> BuiltInEffectList()
    {
        return scBuiltInEffects;
    }

    const BuiltInEffectInfo* FindBuiltInEffect(std::wstring_view nameOrId)
    {
        for (const auto& info : scBuiltInEffects)
            if (SameName(info.name, nameOrId) || info.effectId == nameOrId) return &info;
        return nullptr;
    }

    size_t BuiltInEffectOrder(std::wstring_view effectId)
    {
        for (size_t i = 0; i < std::size(scBuiltInEffects); ++i)
            if (scBuiltInEffects[i].effectId == effectId) return i;
        return SIZE_MAX;
    }

    std::string EffectSymbol(std::wstring_view name)
    {
        std::string symbol;
        for (auto word : Words(name))
        {
            for (auto& c : word) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (word[0] >= 'a' && word[0] <= 'z') word[0] = static_cast<char>(word[0] - 'a' + 'A');
            symbol += word;
        }
        return symbol;
    }

    std::string EffectHeaderName(std::wstring_view name)
    {
        std::string file;
        for (auto word : Words(name))
        {
            for (auto& c : word) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (!file.empty()) file += '_';
            file += word;
        }
        return file + ".effect.h";
    }

    std::string EffectClsidName(std::wstring_view name)
    {
        return "CLSID_EffectGraph" + EffectSymbol(name);
    }

    GUID EffectClsid(std::wstring_view effectId)
    {
        const std::string text = WideToUtf8(effectId);
        uint8_t digest[20]{};
        BCRYPT_HASH_HANDLE hash{};
        bool ok = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)) &&
                  BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(scClsidNamespace), sizeof(scClsidNamespace), 0)) &&
                  BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                                                static_cast<ULONG>(text.size()), 0)) &&
                  BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
        if (hash) BCryptDestroyHash(hash);
        if (!ok) return GUID_NULL;
        digest[6] = static_cast<uint8_t>((digest[6] & 0x0F) | 0x50);
        digest[8] = static_cast<uint8_t>((digest[8] & 0x3F) | 0x80);
        GUID id{};
        id.Data1 = (uint32_t{ digest[0] } << 24) | (uint32_t{ digest[1] } << 16) | (uint32_t{ digest[2] } << 8) | digest[3];
        id.Data2 = static_cast<uint16_t>((digest[4] << 8) | digest[5]);
        id.Data3 = static_cast<uint16_t>((digest[6] << 8) | digest[7]);
        std::memcpy(id.Data4, digest + 8, 8);
        return id;
    }

    std::wstring MissingBuiltInMessage(std::wstring_view nameOrId)
    {
        const auto* info = FindBuiltInEffect(nameOrId);
        if (!info || !info->shader) return {};
        return std::wstring(L"'") + std::wstring(info->name) + L"' is a built-in effect this program did not include: "
               L"include \"" + Utf8ToWide(EffectHeaderName(info->name)) + L"\" (or EffectGraphBuiltIns.h)";
    }

    std::wstring MissingBuiltInMessage(const GUID& clsid)
    {
        for (const auto& info : scBuiltInEffects)
            if (info.shader && EffectClsid(info.effectId) == clsid) return MissingBuiltInMessage(info.effectId);
        return {};
    }
}
