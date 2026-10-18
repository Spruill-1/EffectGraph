#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/BytecodeCache.h"
#include "Effects/EffectBlob.h"
#include "Effects/ImageLoader.h"
#include "Effects/NodeCatalog.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/ShaderVariants.h"
#include "Engine/Api/EffectGraph.h"
#include "Engine/Utf8.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>

// Effect blobs: built from an effect's saved JSON, registered without
// compiling, served from the cache, and falling back -- without compiling --
// when a build the program asks for is not in the blob.
namespace
{
    using namespace ShaderLab::Tests;
    namespace Effects = ShaderLab::Effects;
    namespace Graph = ShaderLab::Graph;

    // A user effect made from the ICtCp tone mapper, whose SourcePeakNits is
    // gpu-bindable. Its own id, and a comment that gives it its own source
    // hash: registering a blob gates compiles of that source, so it must not
    // be the built-in's.
    std::wstring BlobEffectJson(const wchar_t* name)
    {
        const auto* desc = Effects::ShaderLabEffects::Instance().FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        auto node = Effects::ShaderLabEffects::CreateNode(*desc);
        Graph::RenameNode(node, name);
        node.customEffect->shaderLabEffectId = name;
        node.customEffect->hlslSource += L"\n// " + std::wstring(name) + L"\n";
        Graph::GraphModel graph;
        graph.AddNode(std::move(node));
        return std::wstring(graph.ToJson());
    }

    size_t CompileCount()
    {
        const auto stats = Effects::BytecodeCache::Instance().GetStats();
        return stats.inlineCompiles + stats.workerCompiles;
    }

    // Gradient -> tone mapper, with Luminance Statistics bound to SourcePeakNits.
    struct BoundChain
    {
        Graph::GraphModel graph;
        Effects::SourceNodeFactory sources;
        uint32_t toneMap{ 0 };
    };

    std::unique_ptr<BoundChain> MakeBoundChain(const wchar_t* effectName)
    {
        auto chain = std::make_unique<BoundChain>();
        auto& registry = Effects::ShaderLabEffects::Instance();
        auto& g = chain->graph;
        const auto gradient = g.AddNode(Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator")));
        Graph::SetNodeProperty(g, gradient, L"GradSize", 64.0f);
        const auto stats = g.AddNode(Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Luminance Statistics")));
        auto created = Effects::CreateNodeByName(effectName, {}, g);
        if (!created.node) return nullptr;
        chain->toneMap = g.AddNode(std::move(*created.node));
        g.Connect(gradient, 0, stats, 0);
        g.Connect(gradient, 0, chain->toneMap, 0);
        if (!g.BindProperty(chain->toneMap, L"SourcePeakNits", stats, L"Max").empty()) return nullptr;
        return chain;
    }

    // Two frames: the second runs with this frame's statistics bound.
    void RenderTwice(Graph::GraphModel& graph)
    {
        graph.MarkAllDirty();
        RunFrame(graph);
        graph.MarkAllDirty();
        RunFrame(graph);
    }

    SL_TEST_CASE(TestEffectBlobs, Tag::Gpu)
    {
        printf("\n=== Effect blobs ===\n");
        auto& registry = Effects::ShaderLabEffects::Instance();

        // With the GPU-bound shape.
        const auto withShape = Effects::BuildEffectBlob(BlobEffectJson(L"Blob Test Tone Map"), L"blob_test.json",
                                                        { { L"SourcePeakNits" } });
        const size_t optionCombos = 1 + Effects::AllOptionKeys(
            *Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)")).customEffect).size();
        TEST("Blob_Builds", withShape.builds.size() == 2 * optionCombos);
        if (withShape.builds.empty()) { printf("    (debug) %ls\n", withShape.error.c_str()); return; }
        TEST("Blob_UnknownGpuParameterRefused",
             Effects::BuildEffectBlob(BlobEffectJson(L"Blob Bad Param"), L"bad.json", { { L"NoSuchParameter" } }).builds.empty());
        const auto withShapeFile = Effects::EffectBlobFile(withShape);

        // Registering decompresses nothing; builds load on first use.
        const size_t bytesBefore = Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes;
        const auto registered = Effects::RegisterEffectBlob(withShapeFile);
        TEST("Blob_Registers", registered.status == Effects::EffectBlobStatus::Registered &&
             registered.effects == std::vector<std::wstring>{ L"Blob Test Tone Map" } && registry.FindByName(L"Blob Test Tone Map"));
        TEST("Blob_RegisteringDecompressesNothing",
             Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes == bytesBefore);
        TEST("Blob_SecondRegistrationRejected",
             Effects::RegisterEffectBlob(withShapeFile).status == Effects::EffectBlobStatus::NotRegistered);

        // The header: every build behind its macros.
        const auto header = Effects::GenerateEffectHeader(withShape, "cBlobTest", L"blob_test.json");
        TEST("Blob_HeaderSelectsEffect", header.find(
            "#if !defined(EFFECTGRAPH_COMPILE_ONLY_REFERENCED) || (defined(EFFECTGRAPH_BLOB_TEST_TONE_MAP))") != std::string::npos);
        TEST("Blob_HeaderSelectsLayout", header.find(
            "defined(EFFECTGRAPH_BLOB_TEST_TONE_MAP) && defined(EFFECTGRAPH_BLOB_TEST_TONE_MAP_GPU_SOURCEPEAKNITS)") != std::string::npos);
        TEST("Blob_HeaderHasSource", header.find("inline constexpr EFFECTGRAPH_EFFECT_SOURCE cBlobTest = {") != std::string::npos);

        // The gradient and statistics compile on their first use in a run;
        // render them once so the counts below see only the blob's effect.
        if (auto warm = MakeBoundChain(L"ICtCp Tone Map (HDR -> SDR)"))
            RenderTwice(warm->graph);
        CaseEvaluator().ReleaseCache();

        // Used with its statistics bound on the GPU: served from the blob.
        const size_t before = CompileCount();
        const size_t bytesBeforeUse = Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes;
        auto chain = MakeBoundChain(L"Blob Test Tone Map");
        TEST("Blob_ChainBuilt", chain != nullptr);
        if (!chain) return;
        RenderTwice(chain->graph);
        const auto* toneNode = chain->graph.FindNode(chain->toneMap);
        TEST("Blob_Renders", toneNode->cachedOutput && toneNode->runtimeError.empty());
        TEST("Blob_GpuShapeServed", CaseEvaluator().AppliedGpuBits(chain->toneMap) != 0);
        TEST("Blob_NothingCompiled", CompileCount() == before);
        const size_t loaded = Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes - bytesBeforeUse;
        TEST("Blob_OnlyUsedBuildsDecompressed", loaded > 0 && loaded < withShape.BytecodeBytes());
        auto reference = ShaderLab::Rendering::ReadPixelRegion(chain->graph, chain->toneMap, 40, 4, 4, 1, g_dc.get());

        // A blob without that shape: the same render, falling back to the
        // CPU-served binding, still without compiling.
        const auto withoutShape = Effects::BuildEffectBlob(BlobEffectJson(L"Blob Test Tone Map B"), L"blob_b.json", {});
        TEST("Blob_GenericOnlyBuilds", withoutShape.builds.size() == optionCombos);
        Effects::RegisterEffectBlob(Effects::EffectBlobFile(withoutShape));
        // A fresh evaluator: its variant memo is keyed by bytecode identity,
        // and B compiles to the same DXBC as A, so A's GPU build would serve.
        ResetCaseEvaluator();
        const size_t beforeFallback = CompileCount();
        auto fallback = MakeBoundChain(L"Blob Test Tone Map B");
        if (!fallback) { TEST("Blob_FallbackChainBuilt", false); return; }
        RenderTwice(fallback->graph);
        TEST("Blob_FallbackOnCpu", CaseEvaluator().AppliedGpuBits(fallback->toneMap) == 0 &&
             fallback->graph.FindNode(fallback->toneMap)->cachedOutput);
        TEST("Blob_FallbackCompiledNothing", CompileCount() == beforeFallback);
        auto fallbackPixels = ShaderLab::Rendering::ReadPixelRegion(fallback->graph, fallback->toneMap, 40, 4, 4, 1, g_dc.get());
        bool same = reference.pixels.size() == fallbackPixels.pixels.size() && !reference.pixels.empty();
        for (size_t i = 0; same && i < reference.pixels.size(); ++i)
            same = std::abs(reference.pixels[i] - fallbackPixels.pixels[i]) < 1e-4f;
        TEST("Blob_FallbackMatchesGpuShape", same);

        // With runtime compiles allowed, the missing shape is compiled.
        ResetCaseEvaluator();
        auto options = CaseEvaluator().Options();
        options.compilePrebuiltEffects = true;
        CaseEvaluator().SetOptions(options);
        const size_t beforeAllowed = CompileCount();
        RenderTwice(fallback->graph);
        TEST("Blob_AllowedCompilesShape", CaseEvaluator().AppliedGpuBits(fallback->toneMap) != 0 && CompileCount() > beforeAllowed);
        CaseEvaluator().FollowDefaultOptions();

        // Damaged and foreign blobs.
        auto tampered = withShapeFile;
        tampered[8] ^= 0xFF;   // the include-library hash
        TEST("Blob_OtherHeadersIncompatible",
             Effects::RegisterEffectBlob(tampered).status == Effects::EffectBlobStatus::Incompatible);
        const std::vector<uint8_t> truncated(withShapeFile.begin(), withShapeFile.begin() + withShapeFile.size() / 2);
        TEST("Blob_TruncatedCorrupt", Effects::RegisterEffectBlob(truncated).status == Effects::EffectBlobStatus::Corrupt);
        const std::vector<uint8_t> garbage{ 'n', 'o', 'p', 'e', 0, 0, 0, 0 };
        TEST("Blob_GarbageCorrupt", Effects::RegisterEffectBlob(garbage).status == Effects::EffectBlobStatus::Corrupt);

        // A header's parts, as EffectGraphRegisterEffects takes them: first
        // with the generic build left out (nothing to register), then whole.
        const auto forHeader = Effects::BuildEffectBlob(BlobEffectJson(L"Blob Test Tone Map Header"), L"header.json", {});
        std::vector<EFFECTGRAPH_BUILD> partBuilds;
        for (const auto& info : forHeader.builds)
            if (!info.IsGeneric()) partBuilds.push_back({ info.record.data(), static_cast<UINT32>(info.record.size()) });
        EFFECTGRAPH_EFFECT_SOURCE source{ Effects::cEffectBlobFormat, Effects::IncludeLibraryHash(),
            forHeader.description.data(), static_cast<UINT32>(forHeader.description.size()),
            partBuilds.data(), static_cast<UINT32>(partBuilds.size()) };
        TEST("Blob_PartsWithoutGenericRegisterNothing", EffectGraphRegisterEffects(&source) == S_FALSE &&
             !registry.FindByName(L"Blob Test Tone Map Header"));
        partBuilds.clear();
        for (const auto& info : forHeader.builds)
            partBuilds.push_back({ info.record.data(), static_cast<UINT32>(info.record.size()) });
        source.builds = partBuilds.data();
        source.buildCount = static_cast<UINT32>(partBuilds.size());
        TEST("Blob_PartsRegister", EffectGraphRegisterEffects(&source) == S_OK &&
             registry.FindByName(L"Blob Test Tone Map Header"));

        // The public entry point.
        const auto forApi = Effects::BuildEffectBlob(BlobEffectJson(L"Blob Test Tone Map Api"), L"api.json", {});
        const auto forApiFile = Effects::EffectBlobFile(forApi);
        TEST("Blob_ApiRegisters", EffectGraphRegisterEffect(forApiFile.data(), static_cast<UINT32>(forApiFile.size())) == S_OK);
        TEST("Blob_ApiIncompatible", EffectGraphRegisterEffect(tampered.data(), static_cast<UINT32>(tampered.size())) ==
                                         EFFECTGRAPH_E_INCOMPATIBLE_EFFECT);
        TEST("Blob_ApiGarbage", EffectGraphRegisterEffect(garbage.data(), static_cast<UINT32>(garbage.size())) ==
                                    HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        winrt::com_ptr<IEffectGraphNode> node;
        TEST("Blob_ApiCreates", SUCCEEDED(context->CreateEffect(L"Blob Test Tone Map Api", node.put())) && node);
        CaseEvaluator().ReleaseCache();
    }

    std::wstring Env(const wchar_t* name)
    {
        wchar_t value[1024]{};
        const DWORD length = GetEnvironmentVariableW(name, value, 1024);
        return length > 0 && length < 1024 ? std::wstring(value) : std::wstring();
    }

    // A real effect's blob, through the public API on a real HDR image: it
    // registers, renders with both statistics bindings on the GPU, compiles
    // nothing, and its generic build is byte-identical to a fresh compile of
    // the same .json. SHADERLAB_BLOB, SHADERLAB_BLOB_JSON, SHADERLAB_BLOB_IMAGE.
    SL_TEST_CASE(TestEffectBlobRealEffect, Tag::Manual)
    {
        printf("\n=== Effect blob: a real effect ===\n");
        const auto blobPath = Env(L"SHADERLAB_BLOB"), jsonPath = Env(L"SHADERLAB_BLOB_JSON"), imagePath = Env(L"SHADERLAB_BLOB_IMAGE");
        if (blobPath.empty() || jsonPath.empty() || imagePath.empty())
        {
            printf("  [info] set SHADERLAB_BLOB, SHADERLAB_BLOB_JSON and SHADERLAB_BLOB_IMAGE\n");
            return;
        }
        std::ifstream blobFile(blobPath, std::ios::binary);
        const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(blobFile)), std::istreambuf_iterator<char>());
        std::ifstream jsonFile(jsonPath, std::ios::binary);
        std::string jsonUtf8((std::istreambuf_iterator<char>(jsonFile)), std::istreambuf_iterator<char>());
        if (jsonUtf8.size() >= 3 && static_cast<unsigned char>(jsonUtf8[0]) == 0xEF) jsonUtf8.erase(0, 3);
        const std::wstring json = ShaderLab::Utf8ToWide(jsonUtf8);

        const size_t before = CompileCount();
        const size_t bytesBefore = Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes;
        const auto start = std::chrono::steady_clock::now();
        const auto registration = Effects::RegisterEffectBlob(blob);
        const double registerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        printf("  [info] blob %zu bytes registered in %.1f ms: %ls\n", blob.size(), registerMs,
               registration.effects.empty() ? registration.error.c_str() : registration.effects.front().c_str());
        TEST("Real_Registers", registration.status == Effects::EffectBlobStatus::Registered);
        if (registration.effects.empty()) return;

        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        winrt::com_ptr<ID2D1Device> d2dDevice;
        context->GetD2DDevice(d2dDevice.put());
        winrt::com_ptr<ID2D1DeviceContext> created;
        winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, created.put()));
        auto dc = created.as<ID2D1DeviceContext5>();
        Effects::ImageLoader loader;
        auto bitmap = loader.LoadFromFile(imagePath, dc.get());
        TEST("Real_ImageLoads", bitmap != nullptr);
        if (!bitmap) return;
        const auto size = bitmap->GetPixelSize();

        winrt::com_ptr<IEffectGraphNode> input, stats, toneMap;
        winrt::check_hresult(context->CreateImageInput(bitmap.get(), input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        TEST("Real_Creates", SUCCEEDED(context->CreateEffect(registration.effects.front().c_str(), toneMap.put())));
        stats->SetInput(0, input.get());
        context->Realize(stats.get(), nullptr);   // the statistics compile on first use in a run
        const size_t beforeRender = CompileCount();
        toneMap->SetInput(0, input.get());
        TEST("Real_Binds", SUCCEEDED(toneMap->Bind(L"SourcePeakNits", stats.get(), L"Max")) &&
                           SUCCEEDED(toneMap->Bind(L"WhiteErrorBudget", stats.get(), L"WhiteErrorBudget")));

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size.width; desc.Height = size.height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> target;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, target.put()));
        const auto renderStart = std::chrono::steady_clock::now();
        HRESULT rendered = context->RealizeTo(toneMap.get(), target.get());
        rendered = SUCCEEDED(rendered) ? context->RealizeTo(toneMap.get(), target.get()) : rendered;   // bindings settle
        const double renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
        float peak = 0;
        toneMap->GetValueByName(L"SourcePeakNits", D2D1_PROPERTY_TYPE_FLOAT, reinterpret_cast<BYTE*>(&peak), sizeof(peak));
        printf("  [info] %ux%u rendered twice in %.1f ms; SourcePeakNits %.1f; compiles %zu\n",
               size.width, size.height, renderMs, peak, CompileCount() - beforeRender);
        printf("  [info] bytecode in memory after rendering: %.2f MB (statistics included)\n",
               (Effects::BytecodeCache::Instance().GetStats().totalBytecodeBytes - bytesBefore) / 1e6);
        TEST("Real_Renders", SUCCEEDED(rendered));
        TEST("Real_NothingCompiled", CompileCount() == beforeRender);
        TEST("Real_BindingArrived", peak > 0.0f);

        // The blob's generic build against a fresh compile of the .json.
        Effects::UserEffectLoadReport report;
        const auto descriptors = Effects::ShaderLabEffects::ParseEffectsFromJson(json, L"reference.json", report);
        if (descriptors.empty()) { TEST("Real_JsonParses", false); return; }
        const auto node = Effects::ShaderLabEffects::CreateNode(descriptors.front());
        const auto identity = Effects::CompileIdentityOf(node);
        auto request = Effects::MakeVariantRequest(*node.customEffect, identity.effectId, identity.effectVersion,
                                                   Effects::ShaderTargetFor(*node.customEffect), 0, 0);
        request.allowPrebuiltCompile = true;
        const auto fromBlob = Effects::BytecodeCache::Instance().TryGet(request.key);
        Effects::BytecodeCache fresh;
        const auto compiled = fresh.GetOrCompile(request);
        TEST("Real_GenericMatchesFreshCompile", fromBlob.status == Effects::BytecodeStatus::Ready &&
             compiled.status == Effects::BytecodeStatus::Ready && fromBlob.bytecode == compiled.bytecode);
    }
}
