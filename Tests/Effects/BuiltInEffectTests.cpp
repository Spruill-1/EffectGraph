#include "pch_engine.h"
#include "../TestSupport.h"
#include "BuiltInEffects/BuiltInEffectTable.h"
#include "Effects/BuiltInEffects.h"
#include "Effects/BytecodeCache.h"
#include "Effects/EffectBlob.h"
#include "Effects/ShaderVariants.h"
#include "Engine/Api/EffectGraph.h"
#include "luminance_statistics.effect.h"

#include <cstdio>

// The built-in shader effects as the program sees them: registered from the
// generated headers the test runner includes, the same as the table they were
// generated from, prebuilt, and created by class id.
namespace
{
    using namespace ShaderLab;
    using namespace ShaderLab::Tests;

    std::wstring NodeJson(Graph::EffectNode node)
    {
        if (node.customEffect) node.customEffect->shaderGuid = GUID_NULL;
        Graph::GraphModel graph;
        graph.AddNode(std::move(node));
        return std::wstring(graph.ToJson());
    }

    SL_TEST_CASE(TestBuiltInEffectsFromHeaders, Tag::Fast)
    {
        printf("\n=== Built-in effects from generated headers ===\n");
        const auto& registry = Effects::ShaderLabEffects::Instance();
        TEST("BuiltIns_HeadersRegisteredCleanly", Effects::QueuedEffectBlobErrors().empty());

        // Every table entry is registered as the built-in it describes.
        const auto table = Effects::BuiltIn::ShaderEffects();
        size_t same = 0, builtIn = 0, prebuilt = 0;
        for (const auto& expected : table)
        {
            const auto* registered = registry.FindById(expected.effectId);
            if (!registered) { printf("  [info] '%ls' is not registered\n", expected.name.c_str()); continue; }
            builtIn += registered->IsUserEffect() ? 0 : 1;
            if (registered->name == expected.name && registered->category == expected.category &&
                registered->subcategory == expected.subcategory &&
                NodeJson(Effects::ShaderLabEffects::CreateNode(*registered)) ==
                    NodeJson(Effects::ShaderLabEffects::CreateNode(expected)))
                ++same;
            else
                printf("  [info] '%ls' differs from the table\n", expected.name.c_str());

            const auto node = Effects::ShaderLabEffects::CreateNode(*registered);
            const auto identity = Effects::CompileIdentityOf(node);
            const auto generic = Effects::MakeVariantRequest(*node.customEffect, identity.effectId, identity.effectVersion,
                                                             Effects::ShaderTargetFor(*node.customEffect), 0, 0);
            prebuilt += Effects::BytecodeCache::Instance().IsPrebuiltSource(generic.key.sourceHash) ? 1 : 0;
        }
        TEST("BuiltIns_AllRegistered", same == table.size() && table.size() == 30);
        TEST("BuiltIns_MarkedBuiltIn", builtIn == table.size());
        TEST("BuiltIns_Prebuilt", prebuilt == table.size());

        // The registry keeps the engine's order, shader or not.
        const auto list = Effects::BuiltInEffectList();
        bool ordered = registry.All().size() >= list.size();
        for (size_t i = 0; ordered && i < list.size(); ++i)
            ordered = registry.All()[i].effectId == list[i].effectId;
        TEST("BuiltIns_EngineOrder", ordered);

        // The C++ hooks a few built-ins carry beyond their HLSL.
        const auto* gamutMap = registry.FindById(L"ICtCp Gamut Map");
        const auto* boundary = registry.FindById(L"ICtCp Boundary");
        const auto* sideBySide = registry.FindById(L"Side by Side");
        TEST("BuiltIns_HooksAttached", gamutMap && gamutMap->deriveConstants && boundary && boundary->deriveConstants &&
             sideBySide && sideBySide->deriveImageOutputSize);

        // A second module including the same header registers nothing new.
        const size_t count = registry.All().size();
        TEST("BuiltIns_SecondRegistrationIsHarmless",
             EffectGraphRegisterEffects(&cLuminanceStatisticsEffect) == S_OK && registry.All().size() == count);
    }

    SL_TEST_CASE(TestBuiltInEffectIds, Tag::Gpu)
    {
        printf("\n=== Built-in effects by class id ===\n");
        // RFC 9562 version 5 of the effect id; the value Python's uuid.uuid5 gives.
        constexpr GUID cExpected = { 0xa343d1e1, 0x2196, 0x5769, { 0xba, 0x9f, 0xda, 0x07, 0x68, 0x90, 0x65, 0xfb } };
        TEST("Clsid_NameBasedUuid", Effects::EffectClsid(L"Luminance Statistics") == cExpected &&
             CLSID_EffectGraphLuminanceStatistics == cExpected);
        TEST("Clsid_Names", Effects::EffectHeaderName(L"ICtCp Tone Map (HDR -> SDR)") == "ictcp_tone_map_hdr_sdr.effect.h" &&
             Effects::EffectClsidName(L"ICtCp Tone Map (HDR -> SDR)") == "CLSID_EffectGraphIctcpToneMapHdrSdr");

        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        winrt::com_ptr<IEffectGraphNode> stats;
        wchar_t name[64]{};
        TEST("Clsid_Creates", SUCCEEDED(context->CreateEffectById(CLSID_EffectGraphLuminanceStatistics, stats.put())) &&
             stats && SUCCEEDED(stats->GetName(name, 64)) && std::wstring(name) == L"Luminance Statistics");
        winrt::com_ptr<IEffectGraphNode> unknown;
        TEST("Clsid_UnknownRefused", context->CreateEffectById(CLSID_D2D1GaussianBlur, unknown.put()) == EFFECTGRAPH_E_UNKNOWN_EFFECT);

        // A built-in no header registered names the header to include.
        const auto message = Effects::MissingBuiltInMessage(L"Luminance Statistics");
        TEST("NotIncluded_NamesHeader", message.find(L"luminance_statistics.effect.h") != std::wstring::npos &&
             Effects::MissingBuiltInMessage(CLSID_EffectGraphLuminanceStatistics) == message);
        TEST("NotIncluded_OnlyForShaderBuiltIns", Effects::MissingBuiltInMessage(L"Clock").empty() &&
             Effects::MissingBuiltInMessage(L"Gaussian Blur").empty());
    }
}
