#pragma once

#include "pch_engine.h"
#include "ShaderLabEffects.h"

// What the engine knows about its built-in effects without their HLSL. A
// built-in shader effect reaches the registry from the header EffectGraphCompile
// generates for it (<file>.effect.h, or EffectGraphBuiltIns.h for all of them),
// which registers it when the program starts. The engine keeps their names and
// ids, so asking for one whose header was not included names the header, and
// the C++ hooks a few carry beyond their HLSL.
namespace ShaderLab::Effects
{
    struct BuiltInEffectInfo
    {
        std::wstring_view name;
        std::wstring_view effectId;
        bool shader{ false };   // false: always registered, no header
    };

    // Every built-in, in the order the registry lists them.
    std::span<const BuiltInEffectInfo> BuiltInEffectList();

    // The built-in with this name (case-insensitive) or effect id; null if none.
    const BuiltInEffectInfo* FindBuiltInEffect(std::wstring_view nameOrId);

    // Where an effect's position in the registry comes from: its index in
    // BuiltInEffectList, or SIZE_MAX for any other effect.
    size_t BuiltInEffectOrder(std::wstring_view effectId);

    // The names a generated header gives an effect: the PascalCase symbol
    // ("ICtCp Tone Map (HDR -> SDR)" -> IctcpToneMapHdrSdr), the header file
    // (ictcp_tone_map_hdr_sdr.effect.h) and its class id constant
    // (CLSID_EffectGraphIctcpToneMapHdrSdr).
    std::string EffectSymbol(std::wstring_view name);
    std::string EffectHeaderName(std::wstring_view name);
    std::string EffectClsidName(std::wstring_view name);

    // An effect's class id, a name-based (version 5) UUID of its effect id,
    // so it survives renames and every engine derives the same one.
    GUID EffectClsid(std::wstring_view effectId);

    // Why a name or class id that no registered effect has cannot be created:
    // for a built-in, the header to include. Empty when it is no built-in.
    std::wstring MissingBuiltInMessage(std::wstring_view nameOrId);
    std::wstring MissingBuiltInMessage(const GUID& clsid);

    // A gamut selector. The label order is the gamut id GamutPrimariesFor and
    // MakeTargetXf take, so every built-in shares this one.
    Graph::ParameterDefinition GamutParameter(const wchar_t* name, float defaultId,
                                                            const wchar_t* visibleWhen = L"");

    // Gives a built-in its C++ hooks (deriveConstants, deriveImageOutputSize).
    void AttachBuiltInHooks(ShaderLabEffectDescriptor& descriptor);
}
