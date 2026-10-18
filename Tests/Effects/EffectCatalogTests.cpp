#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ColorMathCpu.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderLabEffects.h"
#include "Engine/Utf8.h"
#include "Graph/GraphModel.h"
#include "Rendering/D3D11ComputeRunner.h"
#include "Rendering/GpuTimer.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"
#include "Rendering/SourceRuntime.h"
#include "Graph/GraphEdits.h"

#include <algorithm>
#include <cctype>
#include <variant>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// The built-in effect catalog: shader contracts, sources, viewers and
// comparison effects rendered on the device.
namespace
{
    using namespace ShaderLab::Tests;

    // Every built-in pixel shader declares D2D's input signature. A wrong one
    // renders displaced only when the content does not fill D2D's intermediate,
    // and a render test that checks for output and no error cannot see it.
    // Needs no GPU.
    SL_TEST_CASE(TestPixelShaderSignatures, Tag::Fast)
    {
        printf("\n=== Pixel Shader Signatures (D2D contract guard) ===\n");

        // D2D's pixel-shader input signature is
        //   (SV_POSITION, SCENE_POSITION, TEXCOORD0..N)
        // with one TEXCOORD per input. Omitting SCENE_POSITION shifts the
        // binding so a parameter labelled TEXCOORD0 receives the SCENE
        // coordinate; declaring it twice is equally wrong and fxc accepts it
        // silently. Both are invisible at the default source size.
        const auto& all = ShaderLab::Effects::ShaderLabEffects::Instance().All();
        std::wstring missingScene, duplicateScene, sharedTexcoord, badOrder;
        size_t checked = 0;

        for (const auto& e : all)
        {
            if (e.IsUserEffect()) continue;   // only built-ins are pinned here
            if (e.shaderType != ShaderLab::Graph::CustomShaderType::PixelShader) continue;
            if (e.hlslSource.empty()) continue;

            // Look only at the entry point's parameter list.
            const size_t mainPos = e.hlslSource.rfind("float4 main(");
            if (mainPos == std::string::npos) continue;
            const size_t close = e.hlslSource.find(')', mainPos);
            if (close == std::string::npos) continue;
            const std::string sig = e.hlslSource.substr(mainPos, close - mainPos);
            ++checked;

            auto countOf = [&](const char* needle) {
                size_t n = 0, at = 0;
                while ((at = sig.find(needle, at)) != std::string::npos) { ++n; at += 1; }
                return n;
            };
            const size_t scene = countOf("SCENE_POSITION");

            // Collect the DISTINCT TEXCOORD indices rather than counting the
            // substring. A plain count lets TEXCOORD0-declared-twice satisfy a
            // two-input shader, and lets {0,2} or {1} pass with no index 0.
            std::set<int> texIdx;
            for (size_t at = 0; (at = sig.find("TEXCOORD", at)) != std::string::npos; ++at)
            {
                size_t d = at + 8, v = 0; bool any = false;
                while (d < sig.size() && isdigit(static_cast<unsigned char>(sig[d])))
                { v = v * 10 + static_cast<size_t>(sig[d] - '0'); ++d; any = true; }
                if (any) texIdx.insert(static_cast<int>(v));
            }

            // D2D binds POSITIONALLY, so order matters as much as presence: a
            // shader declaring (SV_POSITION, TEXCOORD0, SCENE_POSITION) passes
            // every presence check and still receives the scene coordinate in
            // uv0. Require SCENE_POSITION to appear before the first TEXCOORD.
            const size_t scenePos = sig.find("SCENE_POSITION");
            const size_t firstTex = sig.find("TEXCOORD");
            if (!texIdx.empty() && scene > 0 && scenePos > firstTex)
                badOrder += e.name + L" ";

            if (!texIdx.empty() && scene == 0) missingScene   += e.name + L" ";
            if (scene > 1)                     duplicateScene += e.name + L" ";

            // Indices must be exactly {0 .. inputs-1}: one per input, no gaps,
            // no duplicates, starting at 0.
            if (!e.inputNames.empty())
            {
                std::set<int> want;
                for (size_t k = 0; k < e.inputNames.size(); ++k) want.insert(static_cast<int>(k));
                // CIE Chromaticity Plot legitimately reads no TEXCOORD: it takes
                // the scene coordinate for diagram geometry and derives its own
                // histogram texel coords via GetDimensions(). Recorded rather
                // than silently tolerated -- see BuiltInEffects/BuiltInEffectTable.cpp.
                const bool knownException = (e.name == L"CIE Chromaticity Plot");
                if (!knownException && texIdx != want) sharedTexcoord += e.name + L" ";
            }
        }

        if (!missingScene.empty())
            printf("  missing SCENE_POSITION: %ls\n", missingScene.c_str());
        if (!duplicateScene.empty())
            printf("  duplicate SCENE_POSITION: %ls\n", duplicateScene.c_str());
        if (!sharedTexcoord.empty())
            printf("  TEXCOORD indices not {0..inputs-1}: %ls\n", sharedTexcoord.c_str());
        if (!badOrder.empty())
            printf("  SCENE_POSITION after TEXCOORD: %ls\n", badOrder.c_str());

        // Pin the scan size the way TestEffectCatalogCount pins the catalog:
        // `checked > 0` cannot notice the extraction silently shrinking, which
        // would turn this guard into a no-op that still reports PASS.
        constexpr size_t kExpectedPixelShadersScanned = 14;
        if (checked != kExpectedPixelShadersScanned)
            printf("  scanned %zu pixel shaders, expected %zu -- if you added or removed one, "
                   "update kExpectedPixelShadersScanned; if not, the main() extraction broke.\n",
                   checked, kExpectedPixelShadersScanned);
        TEST("pixel-shader scan covered the expected count",
             checked == kExpectedPixelShadersScanned);
        TEST("every TEXCOORD-taking pixel shader declares SCENE_POSITION", missingScene.empty());
        TEST("no pixel shader declares SCENE_POSITION twice", duplicateScene.empty());
        TEST("TEXCOORD indices are exactly {0..inputs-1}", sharedTexcoord.empty());
        TEST("SCENE_POSITION precedes TEXCOORD (D2D binds positionally)", badOrder.empty());
    }

    // The ShaderLab effect count is quoted in docs/effects/builtin-catalog.md,
    // docs/README.md, docs/development/project-structure.md,
    // docs/architecture/engine-host-split.md and .github/copilot-instructions.md.
    // Adding or removing a built-in fails here as a reminder to update them.
    SL_TEST_CASE(TestEffectCatalogCount, Tag::Fast)
    {
        printf("\n=== Effect Catalog Count (doc drift guard) ===\n");

        // Bump this together with the catalog table + the counts listed above.
        constexpr size_t cExpectedShaderLabEffects = 38;

        const auto& all = ShaderLab::Effects::ShaderLabEffects::Instance().All();
        const size_t builtIns = static_cast<size_t>(std::count_if(all.begin(), all.end(),
            [](const auto& e) { return !e.IsUserEffect(); }));
        if (builtIns != cExpectedShaderLabEffects)
        {
            printf("  registry holds %zu built-in effects, expected %zu -- update "
                   "docs/effects/builtin-catalog.md and the counts in "
                   "docs/README.md, docs/development/project-structure.md, "
                   "docs/architecture/engine-host-split.md and "
                   ".github/copilot-instructions.md, then bump "
                   "cExpectedShaderLabEffects.\n",
                   builtIns, cExpectedShaderLabEffects);
        }
        TEST_EQ("ShaderLab effect count matches the documented catalog",
                builtIns, cExpectedShaderLabEffects);

        // effectId is the stable identity saved in graphs; a duplicate would
        // make effectVersion upgrades ambiguous on load.
        std::set<std::wstring> ids;
        bool unique = true;
        for (const auto& e : all)
            if (!ids.insert(e.effectId).second) unique = false;
        TEST("every effectId is unique", unique);
    }

    SL_TEST_CASE(TestEffectCatalogLookups, Tag::Fast)
    {
        const auto& effects = ShaderLab::Effects::ShaderLabEffects::Instance();
        const std::wstring padded = L"luminance heatmapXYZ";
        const std::wstring_view prefix(padded.data(), std::wstring_view(L"luminance heatmap").size());
        const auto* byName = effects.FindByName(prefix);
        TEST("Catalog_FindByNameHonoursViewLength",
             byName && byName->name == L"Luminance Heatmap");
        TEST("Catalog_FindByNameRejectsLongerText", effects.FindByName(padded) == nullptr);
        const auto* legacy = effects.FindById(L"Perceptual Gamut Map");
        TEST("Catalog_LegacyIdResolves", legacy && legacy->effectId == L"ICtCp Gamut Map");
        TEST("Catalog_UnknownIdIsNull", effects.FindById(L"No Such Effect") == nullptr);
    }

    // Behaviour fingerprint of a built-in: HLSL with comments and whitespace
    // removed, plus the inputs, parameters and analysis fields a saved graph
    // copies. A comment edit leaves it unchanged; anything else changes it.
    uint64_t EffectFingerprint(const ShaderLab::Effects::ShaderLabEffectDescriptor& e)
    {
        uint64_t hash = 14695981039346656037ull;
        const auto feed = [&hash](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        };
        const auto feedText = [&feed](std::wstring_view text) {
            feed(text.data(), text.size() * sizeof(wchar_t));
            feed("\0", 1);
        };

        const std::string& src = e.hlslSource;
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (src.compare(i, 2, "//") == 0)
            {
                while (i < src.size() && src[i] != '\n') ++i;
                continue;
            }
            if (src.compare(i, 2, "/*") == 0)
            {
                const size_t end = src.find("*/", i + 2);
                i = (end == std::string::npos) ? src.size() : end + 1;
                continue;
            }
            if (!std::isspace(static_cast<unsigned char>(src[i])))
                feed(&src[i], 1);
        }

        for (const auto& input : e.inputNames) feedText(input);
        for (const auto& param : e.parameters)
        {
            feedText(param.name);
            feedText(param.typeName);
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::wstring>) feedText(value);
                else if constexpr (std::is_same_v<T, std::vector<float>>) feed(value.data(), value.size() * sizeof(float));
                else feed(&value, sizeof(value));
            }, param.defaultValue);
            for (const auto& label : param.enumLabels) feedText(label);
            feed(&param.gpuBindable, sizeof(bool));
            feed(&param.specialize, sizeof(bool));
        }
        for (const auto& field : e.analysisFields)
        {
            feedText(field.name);
            feed(&field.type, sizeof(field.type));
            feed(&field.arrayLength, sizeof(field.arrayLength));
        }
        return hash;
    }

    struct EffectVersionPin
    {
        const wchar_t* effectId;
        uint32_t       effectVersion;
        uint64_t       fingerprint;
    };

    // One row per built-in. A changed fingerprint means the effect's
    // behaviour or saved shape changed: bump its effectVersion if saved
    // graphs should be offered the update, then paste the printed row here.
    constexpr EffectVersionPin cEffectVersionPins[] = {
        { L"Luminance Heatmap", 3, 0x5ACB2512EA8C7743ull },
        { L"Gamut Highlight", 6, 0xD78D019514C848F1ull },
        { L"Luminance Highlight", 6, 0xF34B2A42B41F58A4ull },
        { L"CIE Histogram", 6, 0x817618B279310679ull },
        { L"CIE Chromaticity Plot", 6, 0x778D260B961D7AA7ull },
        { L"Gamut Source", 5, 0x235CD2E47CBA3284ull },
        { L"Color Checker", 2, 0xD2F8ABB300B7075Eull },
        { L"Zone Plate", 2, 0xCF33F326662D0E6Aull },
        { L"Gradient Generator", 3, 0x1C85E53C8498C678ull },
        { L"HDR Test Pattern", 2, 0xF90F79B06E34D968ull },
        { L"Delta E Comparator", 7, 0xE3D020D6FE04E065ull },
        { L"Nit Map", 2, 0x1D2818C74D493BF0ull },
        { L"Gamut Coverage", 9, 0xEF38D24CAFB3C46Full },
        { L"Gamut Map", 9, 0xF7CD77E4D7E2F5FDull },
        { L"ICtCp Gamut Map", 15, 0xA44AA4DD82B73133ull },
        { L"ICtCp Boundary", 6, 0xB0C2266C80997B6Dull },
        { L"ICtCp Gamut Boundary LUT", 4, 0xC03D1CFA0A0D1EF3ull },
        { L"Gamut LUT Viewer", 1, 0x2EEEF82A56A22961ull },
        { L"ICtCp Round-Trip Validator", 4, 0xAFD165FED4754CD2ull },
        { L"ICtCp Tone Map", 14, 0x9CBE9512DFA39765ull },
        { L"ICtCp Inverse Tone Map", 14, 0x83E85497E8BD90DDull },
        { L"ICtCp Saturation", 3, 0x2AB934A5C1A567A9ull },
        { L"ICtCp Highlight Desaturation", 5, 0x0E64ED204F5922F8ull },
        { L"Split Comparison", 9, 0x50791FF1F3B0C38Aull },
        { L"Side by Side", 2, 0xB38BF5EBBC6D6899ull },
        { L"Channel Statistics", 2, 0x09A4ED7F8CE41F66ull },
        { L"Luminance Statistics", 6, 0x4844A8551BC50130ull },
        { L"Chromaticity Statistics", 2, 0xB07D81B2A945B2B0ull },
        { L"Float Parameter", 1, 0x0A98841B5C185B75ull },
        { L"Integer Parameter", 1, 0xF5FA6BDA7A0873D8ull },
        { L"Toggle Parameter", 1, 0x736789167FAB1588ull },
        { L"Gamut Parameter", 3, 0xD4FF8C5CD29AF6A5ull },
        { L"Clock", 2, 0x49821B64B026D174ull },
        { L"Math Expression", 1, 0xDBC2338808D62D69ull },
        { L"Random", 1, 0xF35E7F4CA4E04981ull },
        { L"ShaderLab Scale", 2, 0xF67DADF5EB636DB3ull },
        { L"Image Info", 1, 0xFAEC9ED574DB33E5ull },
        { L"Working Space", 1, 0x4D0FDDB576F87E43ull },
    };

    SL_TEST_CASE(TestEffectVersionGuard, Tag::Fast)
    {
        // The fingerprint ignores comments and catches a code change.
        auto probe = *ShaderLab::Effects::ShaderLabEffects::Instance().FindById(L"Nit Map");
        const uint64_t baseline = EffectFingerprint(probe);
        probe.hlslSource += "\n// note\n/* block\n note */\n";
        const bool commentIgnored = EffectFingerprint(probe) == baseline;
        probe.hlslSource += "static const float cExtra = 1.0;\n";
        TEST("Catalog_FingerprintIgnoresComments", commentIgnored);
        TEST("Catalog_FingerprintSeesCode", EffectFingerprint(probe) != baseline);

        bool allPinned = true;
        for (const auto& e : ShaderLab::Effects::ShaderLabEffects::Instance().All())
        {
            if (e.IsUserEffect()) continue;
            const uint64_t fingerprint = EffectFingerprint(e);
            const auto* pin = std::find_if(std::begin(cEffectVersionPins), std::end(cEffectVersionPins),
                [&](const EffectVersionPin& p) { return e.effectId == p.effectId; });
            const bool found = pin != std::end(cEffectVersionPins);
            if (found && pin->effectVersion == e.effectVersion && pin->fingerprint == fingerprint)
                continue;
            if (found && pin->fingerprint != fingerprint && pin->effectVersion == e.effectVersion)
                printf("  %ls changed without an effectVersion bump; bump it if saved graphs "
                       "should be offered the update, then repin:\n", e.effectId.c_str());
            printf("        { L\"%ls\", %u, 0x%016llXull },\n",
                   e.effectId.c_str(), e.effectVersion, static_cast<unsigned long long>(fingerprint));
            allPinned = false;
        }
        TEST("Catalog_EffectVersionsPinned", allPinned);
    }

    SL_TEST_CASE(TestSourceEffects, Tag::Gpu)
    {
        printf("\n=== Source Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        const wchar_t* names[] = {
            L"Gamut Source", L"Color Checker", L"Zone Plate",
            L"Gradient Generator", L"HDR Test Pattern"
        };
        for (const auto* name : names)
        {
            std::string testName = "Source_";
            testName += ShaderLab::WideToUtf8(name);

            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            PrepareSourcesAndEvaluate(g, sf);

            auto* n = g.FindNode(id);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            TEST(testName.c_str(), ok);
        }
    }

    SL_TEST_CASE(TestAnalysisEffects, Tag::Gpu)
    {
        printf("\n=== Analysis Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        // Each graph numbers its nodes from 1, so the evaluator's cache is
        // dropped before each; two frames settle a newly created effect.
        auto renderTwice = [&](ShaderLab::Graph::GraphModel& g,
                               ShaderLab::Effects::SourceNodeFactory& sf) {
            CaseEvaluator().ReleaseCache();
            RenderFrame(g, sf);
            RenderFrame(g, sf);
        };

        const wchar_t* names[] = {
            L"Luminance Heatmap", L"Gamut Highlight", L"Nit Map"
        };
        for (const auto* name : names)
        {
            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            renderTwice(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            std::string testName = "Analysis_";
            testName += ShaderLab::WideToUtf8(name);
            TEST(testName.c_str(), ok);
        }

        {
            auto* desc = registry.FindByName(L"Split Comparison");
            if (!desc) { TEST("FindDescriptor", false); return; }

            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto src1 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto src2 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Color Checker"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto src1Id = g.AddNode(std::move(src1));
            auto src2Id = g.AddNode(std::move(src2));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(src1Id, 0, fxId, 0);
            g.Connect(src2Id, 0, fxId, 1);
            renderTwice(g, sf);

            auto* n = g.FindNode(fxId);
            TEST("Analysis_Split Comparison", n && HasOutput(*n) && n->runtimeError.empty());
        }
    }

    SL_TEST_CASE(TestBuiltInD2DEffects, Tag::Gpu)
    {
        printf("\n=== Built-in D2D Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        const wchar_t* names[] = {
            L"Gaussian Blur", L"Brightness", L"Contrast",
            L"Grayscale", L"Invert", L"Saturation",
            L"Hue Rotation", L"Exposure", L"Sharpen",
            L"Edge Detection", L"Crop"
        };
        for (const auto* name : names)
        {
            auto* desc = d2dReg.FindByName(name);
            if (!desc) { TEST("FindD2DEffect", false); continue; }

            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = d2dReg.CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            PrepareSourcesAndEvaluate(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n);
            std::string testName = "D2D_";
            testName += ShaderLab::WideToUtf8(name);
            TEST(testName.c_str(), ok);
        }
    }

    // Gamut Coverage end to end on a known input, through the multi-group
    // reduction the host runs it with.
    SL_TEST_CASE(TestGamutCoverage, Tag::Gpu)
    {
        printf("\n=== Gamut Coverage ===\n");
        using namespace ShaderLab::Effects;
        const auto* descriptor = ShaderLabEffects::Instance().FindById(L"Gamut Coverage");
        TEST("GamutCoverage_DescriptorExists", descriptor != nullptr);
        if (!descriptor) return;

        ShaderLab::Rendering::D3D11ComputeRunner runner;
        runner.Initialize(g_d3dDevice.get());
        const bool compiled = runner.CompileShader(descriptor->hlslSource);
        if (!compiled) printf("  [info] %ls\n", runner.GetCompileError().c_str());
        TEST("GamutCoverage_Compiles", compiled);
        if (!compiled) return;
        // Install it the way the bridge does; that path detects the reduction scratch.
        const std::vector<uint8_t> bytecode = runner.GetCompiledBytecode();
        winrt::com_ptr<ID3D11ComputeShader> shader;
        g_d3dDevice->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, shader.put());
        runner.InstallPrecompiledShader(bytecode, shader);

        // 256 x 256 source: each of the 64 groups scatters one 1024-pixel
        // block. All transparent (skipped) except 5 pixels of an sRGB colour
        // and 11 of BT.2020 green, spread over many groups, so each bin ends
        // with an exact small count.
        constexpr uint32_t cSourceSize = 256;
        std::vector<float> pixels(cSourceSize * cSourceSize * 4, 0.0f);
        auto setPixel = [&pixels](uint32_t index, float red, float green, float blue)
        {
            pixels[index * 4 + 0] = red;
            pixels[index * 4 + 1] = green;
            pixels[index * 4 + 2] = blue;
            pixels[index * 4 + 3] = 1.0f;
        };
        // (0.6, 0.5, 0.3) is xy (0.358, 0.377): diagram bin (28, 37).
        for (uint32_t index : { 5u, 3u * 1024 + 17, 17u * 1024 + 900, 40u * 1024 + 1, 63u * 1024 + 1023 })
            setPixel(index, 0.6f, 0.5f, 0.3f);
        // BT.2020 green in scRGB is xy (0.170, 0.797): bin (13, 7), outside sRGB.
        for (uint32_t index : { 0u, 1u * 1024 + 2, 2u * 1024 + 3, 9u * 1024, 21u * 1024 + 511, 33u * 1024 + 64,
                                34u * 1024 + 65, 47u * 1024 + 100, 58u * 1024 + 7, 62u * 1024 + 1000, 63u * 1024 + 1022 })
            setPixel(index, -0.5876f, 1.1329f, -0.1006f);

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceDesc.Width = sourceDesc.Height = cSourceSize;
        sourceDesc.MipLevels = sourceDesc.ArraySize = 1;
        sourceDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sourceDesc.SampleDesc.Count = 1;
        sourceDesc.Usage = D3D11_USAGE_DEFAULT;
        sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sourceData{ pixels.data(), cSourceSize * 16, 0 };
        winrt::com_ptr<ID3D11Texture2D> source;
        g_d3dDevice->CreateTexture2D(&sourceDesc, &sourceData, source.put());

        // DiagramSize 64 maps each output pixel to exactly one bin.
        constexpr uint32_t cDiagramSize = 64;
        D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
        outputDesc.Width = outputDesc.Height = cDiagramSize;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        winrt::com_ptr<ID3D11Texture2D> output;
        g_d3dDevice->CreateTexture2D(&outputDesc, nullptr, output.put());
        D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        g_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, staging.put());
        if (!source || !output || !staging)
        {
            TEST("GamutCoverage_Resources", false);
            return;
        }

        // Renders the diagram for one target and returns its pixels.
        auto render = [&](uint32_t targetGamut, const float primaries[6])
        {
            // cbuffer after the injected Width/Height: DiagramSize, TargetGamut, three float2 primaries.
            std::vector<BYTE> constants(8 + 6 * sizeof(float));
            const float diagramSize = static_cast<float>(cDiagramSize);
            memcpy(constants.data(), &diagramSize, 4);
            memcpy(constants.data() + 4, &targetGamut, 4);
            memcpy(constants.data() + 8, primaries, 6 * sizeof(float));
            runner.DispatchWithImageOutput({ source.get() }, constants, 1, output.get());
            std::vector<float> diagram(cDiagramSize * cDiagramSize * 4, -1.0f);
            g_d3dContext->CopyResource(staging.get(), output.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                for (uint32_t row = 0; row < cDiagramSize; ++row)
                    memcpy(diagram.data() + row * cDiagramSize * 4,
                           static_cast<const BYTE*>(mapped.pData) + row * mapped.RowPitch, cDiagramSize * 16);
                g_d3dContext->Unmap(staging.get(), 0);
            }
            return diagram;
        };
        auto binMatches = [](const std::vector<float>& diagram, uint32_t x, uint32_t y,
                             float red, float green, float blue)
        {
            const float* texel = diagram.data() + (y * cDiagramSize + x) * 4;
            const bool match = std::fabs(texel[0] - red) < 1e-5f && std::fabs(texel[1] - green) < 1e-5f &&
                               std::fabs(texel[2] - blue) < 1e-5f && texel[3] == 1.0f;
            if (!match)
                printf("  [info] bin (%u, %u) = (%.5f, %.5f, %.5f, %.5f), expected (%.5f, %.5f, %.5f)\n",
                       x, y, texel[0], texel[1], texel[2], texel[3], red, green, blue);
            return match;
        };

        // Background 0.05; in-gamut hits add (0, 0.8, 0) and out-of-gamut hits
        // (1, 0.2, 0), each times min(hits * 0.05, 1).
        const float srgbPrimaries[6] = { 0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f };
        const auto srgb = render(0, srgbPrimaries);
        TEST("GamutCoverage_Srgb_InGamutBinCountsFive",
             binMatches(srgb, 28, 37, 0.05f, 0.05f + 0.8f * 0.25f, 0.05f));
        TEST("GamutCoverage_Srgb_OutOfGamutBinCountsEleven",
             binMatches(srgb, 13, 7, 0.05f + 0.55f, 0.05f + 0.2f * 0.55f, 0.05f));
        TEST("GamutCoverage_Srgb_EmptyBinIsBackground", binMatches(srgb, 32, 32, 0.05f, 0.05f, 0.05f));

        // A Custom triangle wide enough to hold BT.2020 green: both bins in gamut.
        const float widePrimaries[6] = { 0.75f, 0.25f, 0.05f, 0.95f, 0.10f, 0.02f };
        const auto wide = render(3, widePrimaries);
        TEST("GamutCoverage_Custom_BothBinsInGamut",
             binMatches(wide, 28, 37, 0.05f, 0.05f + 0.8f * 0.25f, 0.05f) &&
             binMatches(wide, 13, 7, 0.05f, 0.05f + 0.8f * 0.55f, 0.05f));
    }

    SL_TEST_CASE(TestEffectChain, Tag::Gpu)
    {
        printf("\n=== Effect Chain ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::GraphModel g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
        auto invertNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Invert"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto blurId = g.AddNode(std::move(blurNode));
        auto invertId = g.AddNode(std::move(invertNode));
        g.Connect(srcId, 0, blurId, 0);
        g.Connect(blurId, 0, invertId, 0);

        PrepareSourcesAndEvaluate(g, sf);
        auto* inv = g.FindNode(invertId);
        TEST("ThreeNodeChain", inv && HasOutput(*inv));
    }

    // Gamut Map's Clip mode behaves like a display: each target channel is
    // clamped to [0, ClipNits / 80], and ClipNits 0 disables the upper clamp.
    SL_TEST_CASE(TestGamutMapClip, Tag::Gpu)
    {
        printf("\n=== Gamut Map: Clip mode clamps both ends, like a display ===\n");
        using ShaderLab::Effects::ShaderLabEffects;
        auto& lib = ShaderLabEffects::Instance();
        const auto* gradDesc = lib.FindByName(L"Gradient Generator");
        const auto* mapDesc = lib.FindByName(L"Gamut Map");
        TEST("GamutClip_EffectsExist", gradDesc && mapDesc);
        if (!gradDesc || !mapDesc) return;

        auto clipOf = [&](float red, float green, float blue, float target, float clipNits) -> std::array<float, 3>
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel graph;
            auto src = graph.AddNode(ShaderLabEffects::CreateNode(*gradDesc));
            auto map = graph.AddNode(ShaderLabEffects::CreateNode(*mapDesc));
            graph.Connect(src, 0, map, 0);
            auto* srcNode = graph.FindNode(src);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"GradSize", 32.0f);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartR", red); ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartG", green); ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartB", blue);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndR", red);   ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndG", green);   ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndB", blue);
            auto* mapNode = graph.FindNode(map);
            ShaderLab::Graph::SetNodeProperty(*mapNode, L"Mode", 0.0f);
            ShaderLab::Graph::SetNodeProperty(*mapNode, L"TargetGamut", target);
            ShaderLab::Graph::SetNodeProperty(*mapNode, L"ClipNits", clipNits);
            graph.MarkAllDirty();
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
            CaseEvaluator().Evaluate(graph, g_dc.get());
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, map, 8, 8, 1, 1, g_dc.get());
            CaseEvaluator().ReleaseCache();
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 3)
                return { -1.0f, -1.0f, -1.0f };
            return { readback.pixels[0], readback.pixels[1], readback.pixels[2] };
        };
        auto approxEq = [](const std::array<float, 3>& actual, float red, float green, float blue) {
            // FP16 intermediates: ~1e-3 relative.
            auto close = [](float x, float y) { return std::abs(x - y) <= 2e-3f + 1e-3f * std::abs(y); };
            return close(actual[0], red) && close(actual[1], green) && close(actual[2], blue);
        };

        // An orange outside sRGB (negative blue) with 960 nits of red, past a
        // 480-nit display.
        auto sdr = clipOf(12.0f, 4.9f, -0.37f, 0.0f, 480.0f);
        printf("  [info] sRGB, ClipNits 480: (%.3f, %.3f, %.3f)\n", sdr[0], sdr[1], sdr[2]);
        TEST("GamutClip_sRGB_ClampsBothEnds", approxEq(sdr, 6.0f, 4.9f, 0.0f));
        auto legacy = clipOf(12.0f, 4.9f, -0.37f, 0.0f, 0.0f);
        TEST("GamutClip_ZeroMeansNoUpperClip_v6Behaviour", approxEq(legacy, 12.0f, 4.9f, 0.0f));
        // Through the P3 matrices, a 960-nit white saturates every channel.
        auto p3White = clipOf(12.0f, 12.0f, 12.0f, 1.0f, 480.0f);
        printf("  [info] P3, ClipNits 480, 960-nit white: (%.3f, %.3f, %.3f)\n", p3White[0], p3White[1], p3White[2]);
        TEST("GamutClip_P3_WhiteSaturatesAtClipNits", approxEq(p3White, 6.0f, 6.0f, 6.0f));
        // Inside both bounds: untouched.
        auto inside = clipOf(1.0f, 0.5f, 0.2f, 1.0f, 480.0f);
        TEST("GamutClip_InRangeUntouched", approxEq(inside, 1.0f, 0.5f, 0.2f));

        // DCI-P3 (4) is adapted to D65, so a 960-nit white clips to a neutral
        // 480 nits. Twice Display P3's green primary is inside Display P3 but
        // outside adapted DCI-P3. Expected values worked in double.
        auto dciWhite = clipOf(12.0f, 12.0f, 12.0f, 4.0f, 480.0f);
        TEST("GamutClip_DciP3_WhiteStaysNeutral", approxEq(dciWhite, 6.0f, 6.0f, 6.0f));
        auto p3Green = clipOf(-0.45f, 2.084f, -0.157f, 1.0f, 480.0f);
        auto dciGreen = clipOf(-0.45f, 2.084f, -0.157f, 4.0f, 480.0f);
        printf("  [info] 2x P3 green: Display P3 (%.4f, %.4f, %.4f), DCI-P3 (%.4f, %.4f, %.4f)\n",
               p3Green[0], p3Green[1], p3Green[2], dciGreen[0], dciGreen[1], dciGreen[2]);
        TEST("GamutClip_P3Green_InsideDisplayP3", approxEq(p3Green, -0.45f, 2.084f, -0.157f));
        TEST("GamutClip_P3Green_ClippedByDciP3", approxEq(dciGreen, -0.30811f, 2.07890f, -0.15624f));

        // A near-black wide-gamut colour (luminance under the near-black
        // cutoff, a negative red) must still leave inside the target.
        auto nearBlack = clipOf(-0.004f, 0.00001f, 0.00002f, 0.0f, 480.0f);
        printf("  [info] near-black wide-gamut through Clip (sRGB): (%.6f, %.6f, %.6f)\n",
               nearBlack[0], nearBlack[1], nearBlack[2]);
        TEST("GamutClip_NearBlackClampedIntoTarget",
             nearBlack[0] >= -1e-6f && nearBlack[1] >= -1e-6f && nearBlack[2] >= -1e-6f);

        // ICtCp Gamut Map has the same near-black early-out.
        const auto* ictcpDesc = lib.FindByName(L"ICtCp Gamut Map");
        TEST("GamutClip_IctcpMapExists", ictcpDesc != nullptr);
        if (ictcpDesc)
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel graph;
            auto src = graph.AddNode(ShaderLabEffects::CreateNode(*gradDesc));
            auto map = graph.AddNode(ShaderLabEffects::CreateNode(*ictcpDesc));
            graph.Connect(src, 0, map, 0);
            auto* srcNode = graph.FindNode(src);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"GradSize", 32.0f);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartR", -0.004f); ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartG", 0.00001f); ShaderLab::Graph::SetNodeProperty(*srcNode, L"StartB", 0.00002f);
            ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndR", -0.004f);   ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndG", 0.00001f);   ShaderLab::Graph::SetNodeProperty(*srcNode, L"EndB", 0.00002f);
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(map), L"TargetGamut", 0.0f);
            graph.MarkAllDirty();
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
            CaseEvaluator().Evaluate(graph, g_dc.get());
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, map, 8, 8, 1, 1, g_dc.get());
            CaseEvaluator().ReleaseCache();
            const bool read = readback.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && readback.pixels.size() >= 3;
            TEST("IctcpMap_NearBlackClampedIntoTarget", read &&
                 readback.pixels[0] >= -1e-6f && readback.pixels[1] >= -1e-6f && readback.pixels[2] >= -1e-6f);
        }
    }

    // DCI-P3 through the LUT generator, and the Gamut LUT Viewer drawing real
    // tables, checked against the tables' own texels.
    SL_TEST_CASE(TestGamutLutViewer, Tag::Gpu)
    {
        printf("\n=== Gamut LUT: DCI-P3 table and the viewer ===\n");
        using ShaderLab::Effects::ShaderLabEffects;
        namespace CM = ShaderLab::Effects::ColorMathCpu;
        auto& library = ShaderLabEffects::Instance();
        const auto* lutDescriptor    = library.FindById(L"ICtCp Gamut Boundary LUT");
        const auto* viewerDescriptor = library.FindById(L"Gamut LUT Viewer");
        const auto* sourceDescriptor = library.FindByName(L"Gamut Source");
        TEST("LutViewer_EffectsExist", lutDescriptor && viewerDescriptor && sourceDescriptor);
        if (!lutDescriptor || !viewerDescriptor || !sourceDescriptor) return;
        TEST("LutViewer_BothInputsAreLookups",
             viewerDescriptor->inputNames.size() == 2 && viewerDescriptor->lookupInputCount == 2);

        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel graph;
        ShaderLab::Effects::SourceNodeFactory sourceFactory;
        auto addNode = [&](const ShaderLab::Effects::ShaderLabEffectDescriptor& descriptor,
                           std::initializer_list<std::pair<const wchar_t*, float>> properties)
        {
            const uint32_t id = graph.AddNode(ShaderLabEffects::CreateNode(descriptor));
            for (const auto& [key, value] : properties)
                ShaderLab::Graph::SetNodeProperty(*graph.FindNode(id), key, value);
            return id;
        };
        constexpr float cExtent = 0.4f;
        constexpr uint32_t cPolarSize = 512;
        const uint32_t lutSrgb = addNode(*lutDescriptor, { { L"TargetGamut", 0.0f } });
        const uint32_t lutP3   = addNode(*lutDescriptor, { { L"TargetGamut", 1.0f } });
        const uint32_t lutDci  = addNode(*lutDescriptor, { { L"TargetGamut", 4.0f } });
        const uint32_t sliceView = addNode(*viewerDescriptor, {
            { L"Mode", 0.0f }, { L"Normalize", 0.0f }, { L"ShowPrimaries", 0.0f },
            { L"OutputWidth", 2048.0f }, { L"OutputHeight", 128.0f } });
        const uint32_t polarView = addNode(*viewerDescriptor, {
            { L"Mode", 1.0f }, { L"Normalize", 1.0f }, { L"ShowPrimaries", 0.0f }, { L"SliceI", 0.5f },
            { L"FitMode", 1.0f }, { L"Extent", cExtent },
            { L"OutputWidth", float(cPolarSize) }, { L"OutputHeight", float(cPolarSize) } });
        const uint32_t unwiredView = addNode(*viewerDescriptor, { { L"OutputWidth", 64.0f }, { L"OutputHeight", 64.0f } });
        const uint32_t imageSource = addNode(*sourceDescriptor, { { L"OutputSize", 128.0f } });
        const uint32_t foreignView = addNode(*viewerDescriptor, { { L"OutputWidth", 64.0f }, { L"OutputHeight", 64.0f } });
        graph.Connect(lutSrgb, 0, sliceView, 0);
        graph.Connect(lutSrgb, 0, polarView, 0);
        graph.Connect(lutDci, 0, polarView, 1);
        graph.Connect(imageSource, 0, foreignView, 0);
        // Only B wired: a node with no input at all is never displayed, so
        // this is how an unwired A is seen.
        graph.Connect(lutDci, 0, unwiredView, 1);

        // The headless frame shape, three times so each table is dispatched
        // before the viewers that read it.
        for (int pass = 0; pass < 3; ++pass)
            RenderFrame(graph, sourceFactory);
        auto read = [&](uint32_t nodeId, int32_t x, int32_t y, uint32_t width = 1, uint32_t height = 1)
        {
            auto result = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, x, y, width, height, g_dc.get());
            if (result.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success ||
                result.pixels.size() < size_t(width) * height * 4)
                return std::vector<float>{};
            return result.pixels;
        };

        // Stamps: same peak, different target fingerprints.
        const auto stampP3 = read(lutP3, 0, 0), stampDci = read(lutDci, 0, 0), stampSrgb = read(lutSrgb, 0, 0);
        const bool stampsRead = !stampP3.empty() && !stampDci.empty() && !stampSrgb.empty();
        TEST("LutDci_TablesRead", stampsRead);
        if (!stampsRead) { CaseEvaluator().ReleaseCache(); return; }
        printf("  [info] stamp alpha: sRGB %.6f, Display P3 %.6f, DCI-P3 %.6f; peak %.3f, I range %.5f\n",
               stampSrgb[3], stampP3[3], stampDci[3], stampDci[1], stampDci[2]);
        TEST("LutDci_StampDiffersFromDisplayP3", std::abs(stampDci[3] - stampP3[3]) > 1e-2f);
        TEST("LutDci_SamePeakAndRange", stampDci[1] == stampP3[1] && stampDci[2] == stampP3[2]);

        // The table geometry, as the header defines it.
        constexpr int cColumns = 2048, cRows = 128;
        const double peakOut = stampSrgb[2];
        const double pi = 3.14159265358979;
        auto readColumn = [&](uint32_t nodeId, int32_t column) { return read(nodeId, column, 0, 1, cRows); };
        auto columnOfHue = [&](double hue) { return int(std::floor((hue / (2.0 * pi) + 0.5) * cColumns)) & (cColumns - 1); };
        auto radiusAt = [&](const std::vector<float>& column, double intensity)
        {
            const double s = 1.0 - std::pow(1.0 - std::clamp(intensity / peakOut, 0.0, 1.0), 0.5);
            const double fy = s * (cRows - 1);
            const int y0 = (std::min)(int(fy), cRows - 2);
            const double t = fy - y0;
            return column[size_t(y0) * 4] * (1.0 - t) + column[size_t(y0 + 1) * 4] * t;
        };
        auto hueOfXy = [](CM::V2 xy)
        {
            const double luminance = 0.5;
            const CM::V3 xyz{ xy.x * luminance / xy.y, luminance, (1.0 - xy.x - xy.y) * luminance / xy.y };
            const CM::V3 ictcp = CM::ScRGBToICtCp(CM::XYZToScRGB(xyz));
            return std::atan2(ictcp.z, ictcp.y);
        };

        // Adapted DCI-P3's green is less saturated than Display P3's and its
        // blue slightly more, its red about the same. The boundary peaks at
        // each corner, so compare the tables across a window of columns
        // around each Display P3 primary's hue, at mid I.
        const double midI = 0.5 * peakOut;
        auto ratioRange = [&](CM::V2 primary)
        {
            constexpr int cHalfWindow = 8, cWindow = 2 * cHalfWindow + 1;
            const int first = columnOfHue(hueOfXy(primary)) - cHalfWindow;
            const auto blockP3 = read(lutP3, first, 0, cWindow, cRows);
            const auto blockDci = read(lutDci, first, 0, cWindow, cRows);
            std::pair<double, double> range{ 1e9, -1e9 };
            if (blockP3.empty() || blockDci.empty()) return range;
            for (int offset = 0; offset < cWindow; ++offset)
            {
                std::vector<float> columnP3(size_t(cRows) * 4), columnDci(size_t(cRows) * 4);
                for (int row = 0; row < cRows; ++row)
                {
                    columnP3[size_t(row) * 4]  = blockP3[(size_t(row) * cWindow + offset) * 4];
                    columnDci[size_t(row) * 4] = blockDci[(size_t(row) * cWindow + offset) * 4];
                }
                const double ratio = radiusAt(columnDci, midI) / radiusAt(columnP3, midI);
                range = { (std::min)(range.first, ratio), (std::max)(range.second, ratio) };
            }
            return range;
        };
        const auto redRange = ratioRange(CM::GAMUT_P3[0]);
        const auto greenRange = ratioRange(CM::GAMUT_P3[1]);
        const auto blueRange = ratioRange(CM::GAMUT_P3[2]);
        printf("  [info] DCI-P3 / Display P3 boundary at mid I near each P3 primary: "
               "red %.4f..%.4f, green %.4f..%.4f, blue %.4f..%.4f\n",
               redRange.first, redRange.second, greenRange.first, greenRange.second, blueRange.first, blueRange.second);
        TEST("LutDci_ShorterAtDisplayP3Green", greenRange.first < 0.95 && greenRange.second <= 1.0);
        TEST("LutDci_LongerAtDisplayP3Blue", blueRange.second > 1.03);
        TEST("LutDci_CloseAtDisplayP3Red", redRange.first > 0.95 && redRange.second < 1.05);

        // Slice map, absolute: each pixel is the colour at (I, B dir(hue)),
        // hue at the column centre, I linear in y. Checked in ICtCp.
        {
            double worst = 0.0;
            bool allRead = true;
            for (const int column : { 300, 1100, 1800 })
            {
                const auto texels = readColumn(lutSrgb, column);
                for (const int y : { 5, 40, 100 })
                {
                    const auto pixel = read(sliceView, column, y);
                    if (texels.empty() || pixel.empty()) { allRead = false; continue; }
                    const double intensity = peakOut * (1.0 - (y + 0.5) / cRows);
                    const double hue = ((column + 0.5) / cColumns) * 2.0 * pi - pi;
                    const double radius = radiusAt(texels, intensity);
                    const CM::V3 ictcp = CM::ScRGBToICtCp({ pixel[0], pixel[1], pixel[2] });
                    worst = (std::max)({ worst, std::abs(ictcp.x - intensity),
                                         std::abs(ictcp.y - radius * std::cos(hue)),
                                         std::abs(ictcp.z - radius * std::sin(hue)) });
                }
            }
            printf("  [info] slice map vs CPU ICtCp of the table texels: max abs err %.3g\n", worst);
            TEST("LutViewer_SliceMapPixelIsBoundaryColour", allRead && worst < 2e-4);
        }

        // Polar slice, fixed extent: along hue 0 (+Ct) the curve lies at the
        // table's radius, halfway between columns 1023 and 1024.
        {
            const double intensity = 0.5 * peakOut;
            const auto left = readColumn(lutSrgb, cColumns / 2 - 1), right = readColumn(lutSrgb, cColumns / 2);
            const auto leftDci = readColumn(lutDci, cColumns / 2 - 1), rightDci = readColumn(lutDci, cColumns / 2);
            const bool columnsOk = !left.empty() && !right.empty() && !leftDci.empty() && !rightDci.empty();
            const double half = 0.5 * cPolarSize;
            const double radiusPx = columnsOk ? 0.5 * (radiusAt(left, intensity) + radiusAt(right, intensity)) / cExtent * half : 0.0;
            const double radiusDciPx = columnsOk ? 0.5 * (radiusAt(leftDci, intensity) + radiusAt(rightDci, intensity)) / cExtent * half : 0.0;
            const int32_t row = int32_t(half) - 1;   // centre 0.5 px above the Ct axis
            auto pixelAt = [&](double fromCentre) { return read(polarView, int32_t(std::lround(half + fromCentre - 0.5)), row); };
            const auto onCurve = pixelAt(radiusPx), inside = pixelAt(radiusPx - 8.0);
            const auto outside = pixelAt((std::max)(radiusPx, radiusDciPx) + 8.0);
            const auto onDci = pixelAt(radiusDciPx);
            printf("  [info] polar: sRGB curve at %.1f px, DCI-P3 at %.1f px from the centre\n", radiusPx, radiusDciPx);
            const bool pixelsOk = columnsOk && radiusPx > 16.0 && (std::max)(radiusPx, radiusDciPx) < half - 16.0 &&
                                  !onCurve.empty() && !inside.empty() && !outside.empty() && !onDci.empty();
            TEST("LutViewer_PolarReadable", pixelsOk);
            if (pixelsOk)
            {
                auto maxChannel = [](const std::vector<float>& p) { return (std::max)({ p[0], p[1], p[2] }); };
                auto minChannel = [](const std::vector<float>& p) { return (std::min)({ p[0], p[1], p[2] }); };
                printf("  [info] polar pixels: on (%.3f %.3f %.3f) inside max %.3f outside max %.3f B (%.3f %.3f %.3f)\n",
                       onCurve[0], onCurve[1], onCurve[2], maxChannel(inside), maxChannel(outside), onDci[0], onDci[1], onDci[2]);
                TEST("LutViewer_PolarCurveAtTableRadius", minChannel(onCurve) > 0.75f);
                TEST("LutViewer_PolarFillInsideCurve", maxChannel(inside) < 0.46f && maxChannel(inside) > 0.05f);
                TEST("LutViewer_PolarBackgroundOutsideCurve", maxChannel(outside) < 0.15f);
                TEST("LutViewer_PolarOverlayCurveAtTableB",
                     std::abs(radiusDciPx - radiusPx) > 4.0 &&
                     std::abs(onDci[0] - 1.0f) < 0.2f && std::abs(onDci[1] - 0.55f) < 0.2f && std::abs(onDci[2] - 0.1f) < 0.2f);
            }
        }

        // No table: grey stripes; an image that is not a table: magenta stripes.
        {
            const auto unwired = read(unwiredView, 0, 0), foreign = read(foreignView, 0, 0);
            TEST("LutViewer_UnwiredShowsGreyStripes",
                 !unwired.empty() && unwired[0] == 0.35f && unwired[1] == 0.35f && unwired[2] == 0.35f);
            TEST("LutViewer_ForeignImageShowsMagentaStripes",
                 !foreign.empty() && foreign[0] == 0.9f && foreign[1] == 0.0f && foreign[2] == 0.9f);
        }
        CaseEvaluator().ReleaseCache();
    }

    // Split Comparison grows and shrinks its input pins with the edges and
    // renders each connected input in an equal wedge around the centre.
    SL_TEST_CASE(TestSplitComparisonWedges, Tag::Gpu)
    {
        printf("\n=== Split Comparison: variadic inputs, equal wedges ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        using ShaderLab::Effects::ShaderLabEffects;

        // Pin count is inputs in use plus one spare, from 2 up to 8.
        {
            ShaderLab::Graph::GraphModel graph;
            auto split = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Split Comparison")));
            std::vector<uint32_t> sources;
            for (int i = 0; i < 8; ++i)
                sources.push_back(graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"))));
            auto pinCount = [&] { return graph.FindNode(split)->inputPins.size(); };
            const size_t atCreate = pinCount();
            graph.Connect(sources[0], 0, split, 0); graph.Connect(sources[1], 0, split, 1);
            const size_t afterTwo = pinCount();
            graph.Connect(sources[2], 0, split, 2);
            const size_t afterThree = pinCount();
            graph.Disconnect(sources[2], 0, split, 2);
            const size_t afterDrop = pinCount();
            graph.Connect(sources[7], 0, split, 7);
            const size_t atCap = pinCount();
            graph.RemoveNode(sources[7]);
            const size_t afterRemove = pinCount();
            printf("  [info] pins: create %zu, 2 in %zu, 3 in %zu, drop %zu, pin 7 %zu, source removed %zu\n",
                   atCreate, afterTwo, afterThree, afterDrop, atCap, afterRemove);
            TEST("Split_PinsStartAtTwo", atCreate == 2);
            TEST("Split_PinsGrowWithASpare", afterTwo == 3 && afterThree == 4);
            TEST("Split_PinsShrinkOnDisconnect", afterDrop == 3);
            TEST("Split_PinsCapAtEight", atCap == 8);
            TEST("Split_PinsShrinkWhenSourceRemoved", afterRemove == 3);
            ShaderLab::Graph::GraphModel back = ShaderLab::Graph::GraphModel::FromJson(graph.ToJson());
            TEST("Split_VariadicSurvivesSaveLoad", back.FindNode(split) && back.FindNode(split)->customEffect->variadicInputs &&
                                                   back.FindNode(split)->inputPins.size() == 3);
        }

        // Render flat red, green and blue 256 px sources.
        const float colours[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        ShaderLab::Graph::GraphModel graph;
        auto split = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Split Comparison")));
        uint32_t sources[3];
        for (int i = 0; i < 3; ++i)
        {
            auto gradient = ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"));
            ShaderLab::Graph::SetNodeProperty(gradient, L"GradSize", 256.0f);
            const wchar_t* keys[] = { L"StartR", L"StartG", L"StartB", L"EndR", L"EndG", L"EndB" };
            for (int k = 0; k < 6; ++k) gradient.properties[keys[k]] = colours[i][k % 3];
            sources[i] = graph.AddNode(std::move(gradient));
            graph.Connect(sources[i], 0, split, static_cast<uint32_t>(i));
        }
        // Which source the pixel at (angle, radius) from the centre shows: the
        // index of its dominant channel, -1 for the white divider, -2 when the
        // read failed.
        auto sourceAt = [&](float angleDeg, float radius) -> int
        {
            CaseEvaluator().ReleaseCache();
            graph.MarkAllDirty();
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
            CaseEvaluator().Evaluate(graph, g_dc.get());
            const float radians = angleDeg * 3.14159265f / 180.0f;
            const int x = static_cast<int>(128 + radius * std::cos(radians));
            const int y = static_cast<int>(128 + radius * std::sin(radians));
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, split, x, y, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 3) return -2;
            const float* rgb = readback.pixels.data();
            if (rgb[0] > 0.9f && rgb[1] > 0.9f && rgb[2] > 0.9f) return -1;
            return static_cast<int>(std::max_element(rgb, rgb + 3) - rgb);
        };
        // Screen angles run clockwise from +x with y down. Three wedges start
        // straight down: 90..210 deg, 210..330, 330..450.
        const int w150 = sourceAt(150, 80), w270 = sourceAt(270, 80), w30 = sourceAt(30, 80);
        printf("  [info] three inputs: 150 deg -> %d, 270 deg -> %d, 30 deg -> %d (0=R 1=G 2=B)\n", w150, w270, w30);
        TEST("Split_ThreeInputsThreeWedges", w150 == 0 && w270 == 1 && w30 == 2);
        TEST("Split_DividerOnWedgeBoundary", sourceAt(90, 60) == -1);   // straight down: first boundary

        // Disconnect the middle input: two halves, no empty wedge.
        graph.Disconnect(sources[1], 0, split, 1);
        const int left = sourceAt(180, 80), right = sourceAt(0, 80);
        TEST("Split_GapSkippedByInputMask", left == 0 && right == 2);

        // Rotation 180 swaps the two halves.
        ShaderLab::Graph::SetNodeProperty(*graph.FindNode(split), L"Rotation", 180.0f);
        TEST("Split_RotationTurnsTheWedges", sourceAt(180, 80) == 2 && sourceAt(0, 80) == 0);
        CaseEvaluator().ReleaseCache();
    }

    // Side by Side: each whole input in its own tile, read exactly at Native.
    SL_TEST_CASE(TestSideBySide, Tag::Gpu | Tag::Timing)
    {
        printf("\n=== Side by Side: whole inputs in tiles ===\n");
        using ShaderLab::Effects::ShaderLabEffects;
        using Float4 = winrt::Windows::Foundation::Numerics::float4;
        const auto* descriptor = ShaderLabEffects::Instance().FindByName(L"Side by Side");
        TEST("SideBySide_Registered", descriptor != nullptr && descriptor->deriveImageOutputSize);
        if (!descriptor || !descriptor->deriveImageOutputSize) return;

        // Output size rule, CPU side.
        {
            const auto node = ShaderLabEffects::CreateNode(*descriptor);
            auto props = node.properties;
            const D2D1_SIZE_U hd{ 1920, 1080 };
            const std::vector<D2D1_SIZE_U> fiveInputs{ hd, hd, { 0, 0 }, hd, hd, hd, { 0, 0 }, { 0, 0 } };
            props[L"Layout"] = 2.0f;
            props[L"Gap"] = 10.0f;
            const auto grid = descriptor->deriveImageOutputSize(props, fiveInputs);
            props[L"Layout"] = 1.0f;
            const auto column = descriptor->deriveImageOutputSize(props, fiveInputs);
            props[L"SizeMode"] = 1.0f;
            const auto fit = descriptor->deriveImageOutputSize(props, fiveInputs);
            printf("  [info] five 1920x1080, gap 10: grid %ux%u, column %ux%u, fit %ux%u\n",
                   grid.width, grid.height, column.width, column.height, fit.width, fit.height);
            TEST("SideBySide_GridSizeFiveIsThreeByTwo", grid.width == 3 * 1920 + 20 && grid.height == 2 * 1080 + 10);
            TEST("SideBySide_ColumnSizeRule", column.width == 1920 && column.height == 5 * 1080 + 40);
            TEST("SideBySide_FitSizeIsOutputSize", fit.width == 1920 && fit.height == 1080);
        }

        // Exact in FP16. Source C is signed, as wide-gamut scRGB is.
        const auto patternA = PatternEffect(L"Side by Side Pattern A",
            "    return float4(0.25 + pixel.x / 256.0, pixel.y / 256.0, 0.5, 1.0);\n");
        const auto patternB = PatternEffect(L"Side by Side Pattern B",
            "    return float4(1.5, 0.125 + pixel.x / 512.0, pixel.y / 128.0, 1.0);\n");
        const auto patternC = PatternEffect(L"Side by Side Pattern C",
            "    return float4(-0.5 - pixel.x / 256.0, 1.25, -0.0625 + pixel.y / 512.0, 1.0);\n");
        constexpr int cTileWidth = 64;
        constexpr int cTileHeight = 32;

        CaseEvaluator().ReleaseCache();
        ShaderLab::Effects::SourceNodeFactory sourceFactory;
        ShaderLab::Graph::GraphModel graph;
        const uint32_t sources[3] = {
            AddPatternSource(graph, sourceFactory, patternA, cTileWidth, cTileHeight),
            AddPatternSource(graph, sourceFactory, patternB, cTileWidth, cTileHeight),
            AddPatternSource(graph, sourceFactory, patternC, cTileWidth, cTileHeight),
        };
        const uint32_t sideBySide = graph.AddNode(ShaderLabEffects::CreateNode(*descriptor));
        for (uint32_t pin = 0; pin < 3; ++pin)
            graph.Connect(sources[pin], 0, sideBySide, pin);
        auto& properties = graph.FindNode(sideBySide)->properties;

        auto render = [&]
        {
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);
        };
        auto pixelAt = [&](uint32_t nodeId, int x, int y) -> std::array<float, 4>
        {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, x, y, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4)
                return { nan, nan, nan, nan };
            return { readback.pixels[0], readback.pixels[1], readback.pixels[2], readback.pixels[3] };
        };
        auto outputSize = [&]() -> std::pair<int, int>
        {
            const auto* node = graph.FindNode(sideBySide);
            if (!node || !node->cachedOutput) return { 0, 0 };
            D2D1_RECT_F bounds{};
            g_dc->GetImageLocalBounds(node->cachedOutput, &bounds);
            return { static_cast<int>(bounds.right - bounds.left), static_cast<int>(bounds.bottom - bounds.top) };
        };
        auto same = [](const std::array<float, 4>& a, const std::array<float, 4>& b, float tolerance)
        {
            for (int channel = 0; channel < 4; ++channel)
                if (!(std::abs(a[channel] - b[channel]) <= tolerance)) return false;
            return true;
        };
        // Corners and centre of `source` against its tile at (left, top).
        auto tileExact = [&](uint32_t source, int left, int top) -> bool
        {
            const int points[5][2] = { { 0, 0 }, { cTileWidth - 1, 0 }, { 0, cTileHeight - 1 },
                                       { cTileWidth - 1, cTileHeight - 1 }, { cTileWidth / 2, cTileHeight / 2 } };
            for (const auto& point : points)
            {
                const auto want = pixelAt(source, point[0], point[1]);
                const auto got = pixelAt(sideBySide, left + point[0], top + point[1]);
                if (std::memcmp(want.data(), got.data(), sizeof(want)) != 0 || std::isnan(want[0]))
                {
                    printf("    (debug) tile at %d,%d point %d,%d: got %.6f %.6f %.6f %.6f, source %.6f %.6f %.6f %.6f\n",
                           left, top, point[0], point[1], got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                    return false;
                }
            }
            return true;
        };
        const std::array<float, 4> black{ 0.0f, 0.0f, 0.0f, 1.0f };

        // Row, no gap.
        render();
        auto size = outputSize();
        printf("  [info] row of three %dx%d: %dx%d, error '%ls'\n", cTileWidth, cTileHeight, size.first, size.second,
               graph.FindNode(sideBySide)->runtimeError.c_str());
        TEST("SideBySide_RowSize", size.first == 3 * cTileWidth && size.second == cTileHeight);
        TEST("SideBySide_RowTilesExact", tileExact(sources[0], 0, 0) && tileExact(sources[1], cTileWidth, 0) &&
                                         tileExact(sources[2], 2 * cTileWidth, 0));
        const auto signedPixel = pixelAt(sideBySide, 2 * cTileWidth + 5, 3);
        TEST("SideBySide_SignedScRgbPreserved", signedPixel[0] < -0.5f && signedPixel[2] < 0.0f);

        // Row with a gap and a background.
        properties[L"Gap"] = 4.0f;
        properties[L"Background"] = Float4{ 0.25f, 0.5f, 0.75f, 1.0f };
        render();
        size = outputSize();
        const std::array<float, 4> background{ 0.25f, 0.5f, 0.75f, 1.0f };
        TEST("SideBySide_GapSize", size.first == 3 * cTileWidth + 8 && size.second == cTileHeight);
        TEST("SideBySide_GapTilesExact", tileExact(sources[0], 0, 0) && tileExact(sources[1], cTileWidth + 4, 0) &&
                                         tileExact(sources[2], 2 * cTileWidth + 8, 0));
        TEST("SideBySide_GapIsBackground", same(pixelAt(sideBySide, cTileWidth, 0), background, 0.0f) &&
                                           same(pixelAt(sideBySide, cTileWidth + 3, cTileHeight - 1), background, 0.0f) &&
                                           same(pixelAt(sideBySide, 2 * cTileWidth + 7, 10), background, 0.0f));
        properties[L"Gap"] = 0.0f;
        properties[L"Background"] = Float4{ 0.0f, 0.0f, 0.0f, 1.0f };

        // Column.
        properties[L"Layout"] = 1.0f;
        render();
        size = outputSize();
        TEST("SideBySide_ColumnSize", size.first == cTileWidth && size.second == 3 * cTileHeight);
        TEST("SideBySide_ColumnTilesExact", tileExact(sources[0], 0, 0) && tileExact(sources[1], 0, cTileHeight) &&
                                            tileExact(sources[2], 0, 2 * cTileHeight));

        // Grid: three inputs in two columns, the fourth cell empty.
        properties[L"Layout"] = 2.0f;
        render();
        size = outputSize();
        TEST("SideBySide_GridSize", size.first == 2 * cTileWidth && size.second == 2 * cTileHeight);
        TEST("SideBySide_GridTilesExact", tileExact(sources[0], 0, 0) && tileExact(sources[1], cTileWidth, 0) &&
                                          tileExact(sources[2], 0, cTileHeight));
        TEST("SideBySide_GridEmptyCellIsBackground", same(pixelAt(sideBySide, cTileWidth + 10, cTileHeight + 10), black, 0.0f));
        properties[L"Layout"] = 0.0f;

        // Unconnected middle pin: two tiles, the third input next to the first.
        graph.Disconnect(sources[1], 0, sideBySide, 1);
        render();
        size = outputSize();
        const float inputMask = std::get<float>(graph.FindNode(sideBySide)->properties[L"InputMask"]);
        TEST("SideBySide_GapPinSkipped", size.first == 2 * cTileWidth && size.second == cTileHeight &&
                                         inputMask == 5.0f && tileExact(sources[2], cTileWidth, 0));
        graph.Connect(sources[1], 0, sideBySide, 1);

        // Fit, enlarging: 192x32 into 384x128 is a scale of 2, letterboxed by
        // 32 rows top and bottom. A linear gradient is exact under bilinear.
        properties[L"SizeMode"] = 1.0f;
        properties[L"OutputWidth"] = 384.0f;
        properties[L"OutputHeight"] = 128.0f;
        render();
        size = outputSize();
        TEST("SideBySide_FitOutputSize", size.first == 384 && size.second == 128);
        {
            // Output (41, 53) is arrangement (20.75, 10.75): texel position 20.25, 10.25 in A.
            const std::array<float, 4> wantA{ 0.25f + 20.25f / 256.0f, 10.25f / 256.0f, 0.5f, 1.0f };
            // Output (161, 53) is arrangement (80.75, 10.75): B at 16.25, 10.25.
            const std::array<float, 4> wantB{ 1.5f, 0.125f + 16.25f / 512.0f, 10.25f / 128.0f, 1.0f };
            const auto gotA = pixelAt(sideBySide, 41, 53);
            const auto gotB = pixelAt(sideBySide, 161, 53);
            printf("  [info] fit x2: A %.6f %.6f (want %.6f %.6f), B %.6f %.6f (want %.6f %.6f)\n",
                   gotA[0], gotA[1], wantA[0], wantA[1], gotB[1], gotB[2], wantB[1], wantB[2]);
            TEST("SideBySide_FitEnlargeBilinear", same(gotA, wantA, 1e-4f) && same(gotB, wantB, 1e-4f));
            TEST("SideBySide_FitLetterbox", same(pixelAt(sideBySide, 100, 10), black, 0.0f) &&
                                            same(pixelAt(sideBySide, 100, 120), black, 0.0f) &&
                                            !same(pixelAt(sideBySide, 100, 33), black, 0.0f));
        }

        // Fit, reducing: 192x32 into 96x16, half size. Each output pixel is
        // the mean of a 2x2 block.
        properties[L"OutputWidth"] = 96.0f;
        properties[L"OutputHeight"] = 16.0f;
        render();
        {
            // Output (10, 5) covers A's texels 20-21, 10-11.
            const std::array<float, 4> want{ 0.25f + 20.5f / 256.0f, 10.5f / 256.0f, 0.5f, 1.0f };
            const auto got = pixelAt(sideBySide, 10, 5);
            printf("  [info] fit x0.5: %.6f %.6f (want %.6f %.6f)\n", got[0], got[1], want[0], want[1]);
            TEST("SideBySide_FitReduceAveragesFootprint", same(got, want, 1e-4f));
        }
        properties[L"SizeMode"] = 0.0f;

        // Mixed sizes: a smaller input centred unscaled, a larger one scaled
        // down to fit, both inside tiles the size of input 0.
        const auto patternWide = PatternEffect(L"Side by Side Pattern Wide",
            "    return float4(pixel.x / 256.0, 0.75, pixel.y / 64.0, 1.0);\n");
        const uint32_t smallSource = AddPatternSource(graph, sourceFactory, patternC, 32, 16);
        const uint32_t wideSource = AddPatternSource(graph, sourceFactory, patternWide, 128, 32);
        graph.Connect(smallSource, 0, sideBySide, 1);
        graph.Connect(wideSource, 0, sideBySide, 2);
        render();
        size = outputSize();
        TEST("SideBySide_MixedSizesKeepTileOfInput0", size.first == 3 * cTileWidth && size.second == cTileHeight);
        {
            // The 32x16 input sits at (16, 8) in tile 1.
            const bool smallPlaced =
                std::memcmp(pixelAt(smallSource, 0, 0).data(), pixelAt(sideBySide, cTileWidth + 16, 8).data(), 16) == 0 &&
                std::memcmp(pixelAt(smallSource, 31, 15).data(), pixelAt(sideBySide, cTileWidth + 47, 23).data(), 16) == 0;
            const bool smallSurround =
                same(pixelAt(sideBySide, cTileWidth + 15, 8), black, 0.0f) &&
                same(pixelAt(sideBySide, cTileWidth + 48, 23), black, 0.0f) &&
                same(pixelAt(sideBySide, cTileWidth + 20, 7), black, 0.0f);
            TEST("SideBySide_SmallerInputCentredUnscaled", smallPlaced && smallSurround);

            // The 128x32 input is halved to 64x16 at (0, 8) in tile 2; output
            // (2*64 + 10, 8 + 3) is the mean of its texels 20-21, 6-7.
            const std::array<float, 4> want{ 20.5f / 256.0f, 0.75f, 6.5f / 64.0f, 1.0f };
            const auto got = pixelAt(sideBySide, 2 * cTileWidth + 10, 11);
            printf("  [info] larger input halved: %.6f %.6f %.6f (want %.6f %.6f %.6f)\n",
                   got[0], got[1], got[2], want[0], want[1], want[2]);
            TEST("SideBySide_LargerInputScaledToFit", same(got, want, 1e-4f) &&
                                                      same(pixelAt(sideBySide, 2 * cTileWidth + 10, 7), black, 0.0f) &&
                                                      same(pixelAt(sideBySide, 2 * cTileWidth + 10, 24), black, 0.0f));
        }

        // An output past the D3D11 texture limit is refused, not truncated.
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel bigGraph;
            ShaderLab::Effects::SourceNodeFactory bigFactory;
            const uint32_t left = AddPatternSource(bigGraph, bigFactory, patternA, 9000, 8);
            const uint32_t right = AddPatternSource(bigGraph, bigFactory, patternB, 9000, 8);
            const uint32_t big = bigGraph.AddNode(ShaderLabEffects::CreateNode(*descriptor));
            bigGraph.Connect(left, 0, big, 0);
            bigGraph.Connect(right, 0, big, 1);
            bigGraph.MarkAllDirty();
            RunFrame(bigGraph);
            bigGraph.MarkAllDirty();
            RunFrame(bigGraph);
            const auto& error = bigGraph.FindNode(big)->runtimeError;
            printf("  [info] 2 x 9000 wide: '%ls'\n", error.c_str());
            TEST("SideBySide_OversizeOutputRefused", error.find(L"18000x8") != std::wstring::npos &&
                                                     error.find(L"16384") != std::wstring::npos);
        }

        // Reducing a 3840x2160 input into a 1024x1024 tile (16 bilinear taps
        // per output pixel) costs about what copying it 1:1 does, measured as
        // the node's own GPU time.
        if (!g_useWarp)
        {
            constexpr int cRuns = 7;
            constexpr double cMaxReduceToCopyRatio = 4.0;
            ShaderLab::Rendering::GpuTimer timer;
            timer.Initialize(g_d3dDevice.get());
            timer.SetEnabled(true);
            auto medianNodeMs = [&](bool reduce)
            {
                CaseEvaluator().ReleaseCache();
                ShaderLab::Graph::GraphModel costGraph;
                ShaderLab::Effects::SourceNodeFactory costFactory;
                const uint32_t large = AddPatternSource(costGraph, costFactory, patternA, 3840, 2160);
                const uint32_t node = costGraph.AddNode(ShaderLabEffects::CreateNode(*descriptor));
                if (reduce)
                {
                    const uint32_t tile = AddPatternSource(costGraph, costFactory, patternB, 1024, 1024);
                    costGraph.Connect(tile, 0, node, 0);
                    costGraph.Connect(large, 0, node, 1);
                }
                else
                {
                    costGraph.Connect(large, 0, node, 0);
                }
                CaseEvaluator().SetGpuTimer(&timer);
                std::vector<double> samples;
                for (int run = 0; run < cRuns + 2; ++run)
                {
                    costGraph.MarkAllDirty();
                    timer.BeginFrame();
                    RunFrame(costGraph);
                    timer.EndFrame();
                    if (timer.CollectBlocking() && run >= 2 && timer.NodeMs(node) > 0.0)
                        samples.push_back(timer.NodeMs(node));
                }
                CaseEvaluator().SetGpuTimer(nullptr);
                std::sort(samples.begin(), samples.end());
                return samples.empty() ? -1.0 : samples[samples.size() / 2];
            };
            const double reduceMs = medianNodeMs(true);
            const double copyMs = medianNodeMs(false);
            printf("  [info] node GPU time: 4K reduced into a 1024 tile %.2f ms, 4K copied 1:1 %.2f ms\n", reduceMs, copyMs);
            TEST("SideBySide_CostMeasured", reduceMs > 0.0 && copyMs > 0.0);
            TIMING_TEST("SideBySide_ReduceCostNearCopy", reduceMs > 0.0 && copyMs > 0.0 &&
                                                                           reduceMs < cMaxReduceToCopyRatio * copyMs);
        }
        CaseEvaluator().ReleaseCache();
    }

    // CIE Histogram: one bin per output pixel, counted exactly against a CPU
    // histogram of the same source pixels.
    SL_TEST_CASE(TestCieHistogram, Tag::Gpu)
    {
        printf("\n=== CIE Histogram: bins at the output resolution ===\n");
        using ShaderLab::Effects::ShaderLabEffects;
        const auto* descriptor = ShaderLabEffects::Instance().FindByName(L"CIE Histogram");
        TEST("CieHist_Registered", descriptor != nullptr);
        if (!descriptor) return;

        // The shader opts into the two-pass image contract.
        {
            ShaderLab::Rendering::D3D11ComputeRunner runner;
            runner.Initialize(g_d3dDevice.get());
            const bool compiled = runner.CompileShader(descriptor->hlslSource);
            if (!compiled) printf("    (debug) compile: %ls\n", runner.GetCompileError().c_str());
            TEST("CieHist_UsesImagePass", compiled && runner.UsesImagePass());
        }

        // Solid patches (one Rec.709 red, one green, one signed wide-gamut),
        // a gradient, a black row (no chromaticity) and transparent rows.
        const auto pattern = PatternEffect(L"CIE Histogram Test Pattern",
            "    if (pixel.y >= 56.0) return float4(0.3, 0.3, 0.3, 0.0);\n"
            "    if (pixel.y < 1.0) return float4(0.0, 0.0, 0.0, 1.0);\n"
            "    if (pixel.x < 24.0) return float4(1.0, 0.0, 0.0, 1.0);\n"
            "    if (pixel.x < 48.0) return float4(0.0, 0.5, 0.0, 1.0);\n"
            "    if (pixel.x < 72.0) return float4(-0.5, 1.25, -0.0625, 1.0);\n"
            "    return float4(0.1 + (pixel.x - 72.0) / 64.0, 0.2 + pixel.y / 256.0, 0.6, 1.0);\n");
        const auto emptyPattern = PatternEffect(L"CIE Histogram Empty Pattern",
            "    return float4(1.0, 0.0, 0.0, 0.0);\n");
        constexpr int cWidth = 96;
        constexpr int cHeight = 64;

        CaseEvaluator().ReleaseCache();
        ShaderLab::Effects::SourceNodeFactory sourceFactory;
        ShaderLab::Graph::GraphModel graph;
        const uint32_t source = AddPatternSource(graph, sourceFactory, pattern, cWidth, cHeight);
        const uint32_t histogram = graph.AddNode(ShaderLabEffects::CreateNode(*descriptor));
        graph.Connect(source, 0, histogram, 0);

        auto render = [&]
        {
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);
        };
        auto readAll = [&](uint32_t nodeId, int width, int height) -> std::vector<float>
        {
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, 0, 0, width, height, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success ||
                readback.actualWidth != static_cast<uint32_t>(width) || readback.actualHeight != static_cast<uint32_t>(height))
                return {};
            return std::move(readback.pixels);
        };

        render();
        const std::vector<float> sourcePixels = readAll(source, cWidth, cHeight);
        TEST("CieHist_SourceReadback", sourcePixels.size() == size_t{ cWidth } * cHeight * 4);
        if (sourcePixels.empty()) return;

        // The shader's mapping, in double. A pixel within float32 error of a
        // bin edge may land on either side, so its bin is excluded from the
        // exact comparison (none are expected for this pattern).
        struct Reference
        {
            std::vector<uint32_t> counts;
            std::vector<double> nitSums;
            std::vector<bool> ambiguous;
            uint64_t total = 0;
            int ambiguousPixels = 0;
        };
        auto reference = [&](int binsPerSide) -> Reference
        {
            Reference result;
            const size_t binCount = size_t(binsPerSide) * binsPerSide;
            result.counts.assign(binCount, 0);
            result.nitSums.assign(binCount, 0.0);
            result.ambiguous.assign(binCount, false);
            for (size_t i = 0; i < size_t{ cWidth } * cHeight; ++i)
            {
                const float* rgba = &sourcePixels[i * 4];
                if (rgba[3] < 0.01f) continue;
                const double x = 0.4123908 * rgba[0] + 0.3575843 * rgba[1] + 0.1804808 * rgba[2];
                const double y = 0.2126390 * rgba[0] + 0.7151687 * rgba[1] + 0.0721923 * rgba[2];
                const double z = 0.0193308 * rgba[0] + 0.1191950 * rgba[1] + 0.9505322 * rgba[2];
                const double sum = x + y + z;
                if (sum < 1e-7) continue;
                const double u = (x / sum - 0.3127) + 0.5;
                const double v = 0.5 - (y / sum - 0.3290);
                if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) continue;
                const double binU = u * binsPerSide;
                const double binV = v * binsPerSide;
                const int binX = (std::min)(static_cast<int>(binU), binsPerSide - 1);
                const int binY = (std::min)(static_cast<int>(binV), binsPerSide - 1);
                const size_t bin = size_t(binY) * binsPerSide + binX;
                ++result.counts[bin];
                result.nitSums[bin] += y * 80.0;
                ++result.total;
                const double edgeTolerance = binsPerSide * 2e-6;
                if (std::abs(binU - std::round(binU)) < edgeTolerance || std::abs(binV - std::round(binV)) < edgeTolerance)
                {
                    result.ambiguous[bin] = true;
                    ++result.ambiguousPixels;
                }
            }
            return result;
        };

        // Compares the histogram at `outputSize` with blocks of `blockSide`
        // output pixels per bin against the reference at the coarse grid.
        auto check = [&](int outputSize, int blockSide, const char* label)
        {
            auto* settings = graph.FindNode(histogram);
            ShaderLab::Graph::SetNodeProperty(*settings, L"OutputSize", static_cast<uint32_t>(outputSize));
            ShaderLab::Graph::SetNodeProperty(*settings, L"BinSize", static_cast<float>(blockSide - 1));
            render();
            // A second frame must not add to the first one's bins.
            render();
            const auto* node = graph.FindNode(histogram);
            const std::vector<float> output = readAll(histogram, outputSize, outputSize);
            const int binsPerSide = (outputSize + blockSide - 1) / blockSide;
            const Reference expected = reference(binsPerSide);
            const auto maxBin = std::max_element(expected.counts.begin(), expected.counts.end());
            const uint32_t maxCount = *maxBin;
            const double logMax = std::log2(double(maxCount) + 1.0);
            auto texelAt = [&](int x, int y) { return &output[(size_t(y) * outputSize + x) * 4]; };

            int countMismatches = 0;
            int occupancyMismatches = 0;
            int unevenBlocks = 0;
            uint64_t gpuTotal = 0;
            float maxR = 0.0f;
            double maxLumError = 0.0;
            bool finite = !output.empty();
            for (size_t bin = 0; bin < expected.counts.size() && !output.empty(); ++bin)
            {
                // Every pixel of the bin's block, clipped at the image edge,
                // must hold the block's first pixel.
                const int left = int(bin % binsPerSide) * blockSide;
                const int top = int(bin / binsPerSide) * blockSide;
                const float* texel = texelAt(left, top);
                bool flat = true;
                for (int y = top; y < (std::min)(top + blockSide, outputSize); ++y)
                    for (int x = left; x < (std::min)(left + blockSide, outputSize); ++x)
                        flat = flat && std::memcmp(texelAt(x, y), texel, 4 * sizeof(float)) == 0;
                if (!flat) ++unevenBlocks;

                for (int channel = 0; channel < 4; ++channel)
                    finite = finite && std::isfinite(texel[channel]);
                maxR = (std::max)(maxR, texel[0]);
                const bool hit = texel[3] > 0.5f;
                const uint32_t count = hit
                    ? static_cast<uint32_t>(std::llround(std::exp2(double(texel[0]) * logMax) - 1.0)) : 0;
                gpuTotal += count;
                if (hit != (expected.counts[bin] > 0)) ++occupancyMismatches;
                if (expected.ambiguous[bin]) continue;
                if (count != expected.counts[bin]) ++countMismatches;
                if (expected.counts[bin] > 0)
                {
                    const double expectedLum = expected.nitSums[bin] / expected.counts[bin] / 10000.0;
                    maxLumError = (std::max)(maxLumError, std::abs(double(texel[1]) - expectedLum));
                }
            }
            const size_t maxIndex = size_t(maxBin - expected.counts.begin());
            const float rAtMax = output.empty() ? 0.0f
                : texelAt(int(maxIndex % binsPerSide) * blockSide, int(maxIndex / binsPerSide) * blockSide)[0];
            printf("  [info] %d px, %dx%d blocks, %d bins/side: %llu px counted (CPU %llu), max count %u, ambiguous px %d, "
                   "count mismatches %d, occupancy mismatches %d, uneven blocks %d, max R %.7f (at CPU max %.7f), "
                   "max |dG| %.2e, error '%ls'\n",
                   outputSize, blockSide, blockSide, binsPerSide,
                   static_cast<unsigned long long>(gpuTotal), static_cast<unsigned long long>(expected.total),
                   maxCount, expected.ambiguousPixels, countMismatches, occupancyMismatches, unevenBlocks, maxR, rAtMax,
                   maxLumError, node ? node->runtimeError.c_str() : L"(no node)");
            const std::string prefix = std::string("CieHist_") + label;
            TEST((prefix + "_ReadbackFinite").c_str(), finite);
            TEST((prefix + "_ExactCounts").c_str(), finite && countMismatches == 0 && occupancyMismatches == 0 &&
                                                    gpuTotal == expected.total && expected.total > 0);
            TEST((prefix + "_BlocksFlat").c_str(), finite && unevenBlocks == 0);
            TEST((prefix + "_MaxBinIsOne").c_str(), std::abs(maxR - 1.0f) < 1e-6f && std::abs(rAtMax - 1.0f) < 1e-6f);
            TEST((prefix + "_AverageLuminance").c_str(), finite && maxLumError < 1e-6);
        };
        check(64, 1, "64");
        check(256, 1, "256");
        // 64 / 3 and 256 / 10 leave a partial last row and column of blocks.
        check(64, 3, "64Bin3");
        check(64, 10, "64Bin10");
        check(256, 10, "256Bin10");
        check(64, 1, "64Again");

        // A fully transparent source: every bin empty, nothing undefined.
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Effects::SourceNodeFactory emptyFactory;
            ShaderLab::Graph::GraphModel emptyGraph;
            const uint32_t emptySource = AddPatternSource(emptyGraph, emptyFactory, emptyPattern, cWidth, cHeight);
            const uint32_t emptyHistogram = emptyGraph.AddNode(ShaderLabEffects::CreateNode(*descriptor));
            emptyGraph.Connect(emptySource, 0, emptyHistogram, 0);
            ShaderLab::Graph::SetNodeProperty(*emptyGraph.FindNode(emptyHistogram), L"OutputSize", 64u);
            emptyGraph.MarkAllDirty();
            RunFrame(emptyGraph);
            emptyGraph.MarkAllDirty();
            RunFrame(emptyGraph);
            auto readback = ShaderLab::Rendering::ReadPixelRegion(emptyGraph, emptyHistogram, 0, 0, 64, 64, g_dc.get());
            bool allZero = readback.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success &&
                           readback.pixels.size() == size_t{ 64 } * 64 * 4;
            for (float value : readback.pixels)
                allZero = allZero && value == 0.0f;
            TEST("CieHist_EmptyInputAllZero", allZero);
        }
        CaseEvaluator().ReleaseCache();
    }
}
