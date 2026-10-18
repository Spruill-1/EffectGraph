#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// User effects loaded from saved-graph files.
namespace
{
    using namespace ShaderLab::Tests;

    // User effects load from saved-graph files with the documented conflict
    // policy, and bad files are reported rather than registered. Tagged Last:
    // it appends to the process-wide registry.
    SL_TEST_CASE(TestUserEffectLibrary, Tag::Gpu | Tag::Last)
    {
        printf("\n=== User effect library ===\n");
        namespace fs = std::filesystem;
        using ShaderLab::Effects::ShaderLabEffects;
        auto& lib = ShaderLabEffects::Instance();

        const ScopedTempDir tempDir(L"ShaderLabUserEffectsTest");
        const fs::path dir = tempDir.Path();
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);

        // Outputs its Level parameter, so the rendered pixel shows which
        // definition was registered.
        ShaderLab::Effects::ShaderLabEffectDescriptor validDesc;
        validDesc.name = L"Test User Effect";
        validDesc.effectId = L"Test User Effect";
        validDesc.effectVersion = 3;
        validDesc.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        validDesc.inputNames = { L"Source" };
        validDesc.hlslSource = R"HLSL(
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0) { float Level; };
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float a = Source.Sample(InputSampler, uv0.xy).a;
    return float4(Level, Level, Level, a);
}
)HLSL";
        validDesc.parameters = { ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.25f, 0.0f, 1.0f, 0.01f } };

        // Save a graph holding one node of `desc`, with an optional top-level
        // "category" key.
        auto writeEffectFile = [&](const wchar_t* file, const ShaderLab::Effects::ShaderLabEffectDescriptor& desc,
                                   const char* category)
        {
            ShaderLab::Graph::GraphModel graph;
            graph.AddNode(ShaderLabEffects::CreateNode(desc));
            std::wstring json(graph.ToJson());
            if (category)
                json.insert(1, L"\"category\":\"" + std::wstring(category, category + strlen(category)) + L"\",");
            std::ofstream(dir / file, std::ios::binary) << winrt::to_string(json);
        };
        writeEffectFile(L"a_valid.json", validDesc, "Testing");
        auto broken = validDesc; broken.name = broken.effectId = L"Test Broken Effect";
        broken.hlslSource = "float4 main() : SV_TARGET { return nope; }";
        writeEffectFile(L"b_broken.json", broken, nullptr);
        std::ofstream(dir / L"c_garbage.json") << "this is not a graph";
        { ShaderLab::Graph::GraphModel empty; std::ofstream(dir / L"d_empty.json", std::ios::binary) << winrt::to_string(empty.ToJson()); }
        // Same id as a_valid, loaded after it: v9 replaces v3, and v2 must not
        // replace v9.
        auto newer = validDesc; newer.effectVersion = 9;
        newer.parameters[0].defaultValue = 0.75f;
        writeEffectFile(L"e_newer.json", newer, "Testing");
        auto older = validDesc; older.effectVersion = 2;
        writeEffectFile(L"f_older.json", older, nullptr);
        // A user effect named like a built-in is never allowed to replace it.
        auto clash = validDesc; clash.name = L"Luminance Statistics"; clash.effectId = L"Test Clash";
        writeEffectFile(L"g_builtin_clash.json", clash, nullptr);
        std::ofstream(dir / L"ignored.txt") << "not json, not scanned";

        const uint32_t libVersionBefore = lib.LibraryVersion();
        const size_t errorsBefore = lib.UserEffectReport().errors.size();
        const auto& report = lib.LoadUserEffects(dir.wstring());
        auto errorFor = [&](const wchar_t* file, const wchar_t* needle) {
            for (size_t i = errorsBefore; i < report.errors.size(); ++i)
                if (report.errors[i].rfind(file, 0) == 0 && report.errors[i].find(needle) != std::wstring::npos)
                    return true;
            return false;
        };
        for (size_t i = errorsBefore; i < report.errors.size(); ++i)
            printf("  [info] %ls\n", report.errors[i].c_str());

        const auto* userEffect = lib.FindByName(L"Test User Effect");
        TEST("UserFx_ValidEffectRegistered", userEffect != nullptr && userEffect->IsUserEffect());
        TEST("UserFx_CategoryFromFile", userEffect && userEffect->category == L"Testing");
        TEST("UserFx_NewerUserFileWins", userEffect && userEffect->effectVersion == 9 && lib.FindById(L"Test User Effect") == userEffect &&
                                         userEffect->sourcePath.find(L"e_newer.json") != std::wstring::npos);
        TEST("UserFx_OlderUserFileRejected", errorFor(L"f_older.json", L"is not newer than v9"));
        {
            const auto* builtIn = lib.FindByName(L"Luminance Statistics");
            TEST("UserFx_BuiltInNeverReplaced", builtIn && !builtIn->IsUserEffect() && lib.FindById(L"Test Clash") == nullptr &&
                                                errorFor(L"g_builtin_clash.json", L"built-ins are never replaced"));
        }
        {
            // The conflict policy on its own.
            using ShaderLab::Effects::ResolveUserEffectConflict;
            using ShaderLab::Effects::UserEffectConflict;
            ShaderLab::Effects::ShaderLabEffectDescriptor builtIn, userV3, userV9;
            builtIn.effectVersion = 1;
            userV3.effectVersion = 3; userV3.sourcePath = L"a.json";
            userV9.effectVersion = 9; userV9.sourcePath = L"b.json";
            auto userV9Again = userV9; userV9Again.sourcePath = L"c.json";
            TEST("UserFxPolicy_BuiltInBeatsAnyVersion", ResolveUserEffectConflict(builtIn, userV9) == UserEffectConflict::Reject);
            TEST("UserFxPolicy_HigherVersionReplaces", ResolveUserEffectConflict(userV3, userV9) == UserEffectConflict::Replace);
            TEST("UserFxPolicy_LowerVersionRejected",  ResolveUserEffectConflict(userV9, userV3) == UserEffectConflict::Reject);
            TEST("UserFxPolicy_TieKeepsFirst",         ResolveUserEffectConflict(userV9, userV9Again) == UserEffectConflict::Reject);
        }
        TEST("UserFx_BrokenHlslReportedNotAdded",
             lib.FindByName(L"Test Broken Effect") == nullptr && errorFor(L"b_broken.json", L"does not compile"));
        TEST("UserFx_GarbageReported", errorFor(L"c_garbage.json", L""));
        TEST("UserFx_NoShaderNodesReported", errorFor(L"d_empty.json", L"no shader nodes"));
        TEST("UserFx_NonJsonIgnored", !errorFor(L"ignored.txt", L""));
        {
            size_t registrations = 0;
            for (const auto& e : lib.All()) if (e.effectId == L"Test User Effect") ++registrations;
            TEST("UserFx_ConflictNeverRegistersTwice", registrations == 1);
        }
        TEST("UserFx_LibraryVersionCountsBuiltInsOnly", lib.LibraryVersion() == libVersionBefore);
        {
            const size_t errorsBeforeMissing = lib.UserEffectReport().errors.size();
            lib.LoadUserEffects((dir / L"does-not-exist").wstring());
            TEST("UserFx_MissingDirectoryIsNotAnError", lib.UserEffectReport().errors.size() == errorsBeforeMissing);
        }

        // Render the registered effect; 0.75 is the v9 file's default Level.
        float renderedLevel = -1.0f;
        if (userEffect)
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel graph;
            auto source = graph.AddNode(ShaderLabEffects::CreateNode(*lib.FindByName(L"Gamut Source")));
            auto node = graph.AddNode(ShaderLabEffects::CreateNode(*userEffect));
            graph.Connect(source, 0, node, 0);
            graph.MarkAllDirty();
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
            CaseEvaluator().Evaluate(graph, g_dc.get());
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, node, 8, 8, 1, 1, g_dc.get());
            if (readback.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && !readback.pixels.empty())
                renderedLevel = readback.pixels[0];
            CaseEvaluator().ReleaseCache();
        }
        TEST("UserFx_RendersItsOwnDefinition", std::abs(renderedLevel - 0.75f) < 1e-3f);

        fs::remove_all(dir, ec);   // best-effort cleanup
    }
}
