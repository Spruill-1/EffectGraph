#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/BytecodeCache.h"
#include "Effects/ShaderLabParamsHlsl.h"
#include "Effects/ShaderVariants.h"
#include "Engine/Utf8.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// The bytecode cache: memory and disk tiers, workers and queue order.
namespace
{
    using namespace ShaderLab::Tests;

    SL_TEST_CASE(TestBytecodeCache, Tag::Fast)
    {
        printf("\n=== BytecodeCache ===\n");
        using namespace ShaderLab::Effects;
        auto& cache = BytecodeCache::Instance();
        cache.Clear();

        // NOTE: the inline HLSL in this test is a COMPILE/CACHE-KEY fixture,
        // not a signature example. It is never rasterised, so its
        // two-parameter main() is harmless here -- but do not copy it. The
        // real contract is (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), one
        // TEXCOORD per input; TestPixelShaderSignatures enforces it over the
        // shipped catalog.
        std::string hlsl = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        std::string canonical = CanonicalizeHlslSource(hlsl);

        BytecodeCompileRequest req;
        req.key.sourceHash         = HashCanonicalSource(canonical);
        req.key.paramSignatureHash = HashParamSignature({});
        req.key.includeLibraryHash = IncludeLibraryHash();
        req.key.macroBitset        = 0;
        req.key.entryPoint         = "main";
        req.key.target             = "ps_5_0";
        req.metadata.effectId      = L"BytecodeCacheTest";
        req.metadata.version       = 1;
        req.hlslSource             = canonical;

        // First call -> miss -> inline compile -> Ready.
        auto r1 = cache.GetOrCompile(req);
        TEST("BytecodeCache_FirstCompileReady", r1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_FirstCompileBytecodeNonEmpty", !r1.bytecode.empty());
        TEST("BytecodeCache_FirstCompileNotFromCache", !r1.fromCache);

        // Second call with identical key -> cache hit, identical bytecode.
        auto r2 = cache.GetOrCompile(req);
        TEST("BytecodeCache_SecondCompileReady", r2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_SecondCompileFromCache", r2.fromCache);
        TEST("BytecodeCache_SecondCompileBytecodeMatches",
             r2.bytecode.size() == r1.bytecode.size() &&
             memcmp(r2.bytecode.data(), r1.bytecode.data(), r1.bytecode.size()) == 0);

        // Failed entry caches its diagnostic and doesn't recompile.
        BytecodeCompileRequest bad = req;
        bad.hlslSource             = "broken!";
        bad.key.sourceHash         = HashCanonicalSource(bad.hlslSource);
        auto bad1 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFailed", bad1.status == BytecodeStatus::Failed);
        TEST("BytecodeCache_BadCompileHasDiagnostic", !bad1.errorMessage.empty());

        auto bad2 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFromCacheOnRetry", bad2.fromCache);

        // Macro bitset participates in identity: same source + different
        // macro bits should produce a fresh entry, not reuse the
        // baseline.
        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        std::string macroCanonical = CanonicalizeHlslSource(macroPS);
        BytecodeCompileRequest mBase;
        mBase.key.sourceHash         = HashCanonicalSource(macroCanonical);
        mBase.key.paramSignatureHash = HashParamSignature({ "Exposure" });
        mBase.key.includeLibraryHash = IncludeLibraryHash();
        mBase.key.macroBitset        = 0;
        mBase.key.entryPoint         = "main";
        mBase.key.target             = "ps_5_0";
        mBase.hlslSource             = macroCanonical;
        mBase.gpuBindableParamNames  = { "Exposure" };

        auto cb = cache.GetOrCompile(mBase);
        TEST("BytecodeCache_MacroCbufferOK", cb.status == BytecodeStatus::Ready);

        BytecodeCompileRequest mGpu = mBase;
        mGpu.key.macroBitset = 1;
        auto gp = cache.GetOrCompile(mGpu);
        TEST("BytecodeCache_MacroGpuOK", gp.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_MacroVariantsDistinct",
             cb.bytecode.size() != gp.bytecode.size() ||
             memcmp(cb.bytecode.data(), gp.bytecode.data(), cb.bytecode.size()) != 0);

        // PrecompileBindingShapes queues the baseline and one variant per
        // gpu-bindable parameter under the keys the evaluator asks for.
        cache.Clear();
        ShaderLab::Graph::CustomEffectDefinition precompileDef;
        precompileDef.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        precompileDef.hlslSource = ShaderLab::Utf8ToWide(macroPS);
        precompileDef.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Exposure", L"float", 1.0f, 0.0f, 10.0f, 0.01f, {}, L"", true },
        };
        const uint32_t textureBits = WithBindingMode(0, 0, GpuBindingMode::Texture);
        auto precompileRequest = [&](uint32_t bits)
        {
            return MakeVariantRequest(precompileDef, L"PrecompileTest", 1, "ps_5_0", bits, 0);
        };
        PrecompileBindingShapes(cache, precompileDef, L"PrecompileTest", 1, "ps_5_0");
        TEST("BytecodeCache_PrecompileKeysMatchEvaluatorRequests",
             cache.GetStatus(precompileRequest(0).key) != BytecodeStatus::NotRequested &&
             cache.GetStatus(precompileRequest(textureBits).key) != BytecodeStatus::NotRequested);

        // Wait up to 5s for both variants. Drives both the worker
        // queue and (via timeoutMs) the inline-fallback path.
        auto wb = cache.GetOrCompile(precompileRequest(0), 5000);
        auto wg = cache.GetOrCompile(precompileRequest(textureBits), 5000);
        TEST("BytecodeCache_PrecompileBaselineReady", wb.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_PrecompileGpuVariantReady", wg.status == BytecodeStatus::Ready);

        // An effect with specialised options is left to its option variants.
        cache.Clear();
        auto optionDef = precompileDef;
        optionDef.parameters.push_back(ShaderLab::Graph::ParameterDefinition{
            L"Mode", L"uint", 0u, 0.0f, 1.0f, 1.0f, { L"A", L"B" } });
        optionDef.parameters.back().specialize = true;
        PrecompileBindingShapes(cache, optionDef, L"PrecompileTest", 1, "ps_5_0");
        TEST("BytecodeCache_PrecompileSkipsOptionEffects",
             cache.GetStatus(MakeVariantRequest(optionDef, L"PrecompileTest", 1, "ps_5_0", 0, 0).key) ==
                 BytecodeStatus::NotRequested);

        cache.Clear();

        // ---- Disk persistence ----------------------------------------------
        // Configure a temp disk root, compile, verify file written,
        // then clear in-memory and verify GetOrCompile re-reads from
        // disk (not from a fresh D3DCompile). A cache of its own, so the
        // process cache never sees this root.
        const ScopedTempDir diskDir(L"shaderlab_bytecode_cache_test");
        BytecodeCache diskCache;
        std::wstring diskRoot = diskDir.Path().wstring();
        diskCache.SetDiskCacheRoot(diskRoot);
        // Wipe any leftover state from a previous test run -- we want
        // a known-empty disk + memory state for the assertions below.
        diskCache.ClearDisk();
        diskCache.Clear();

        BytecodeCompileRequest diskReq;
        diskReq.key.sourceHash         = HashCanonicalSource(canonical);
        diskReq.key.paramSignatureHash = HashParamSignature({});
        diskReq.key.includeLibraryHash = IncludeLibraryHash();
        diskReq.key.macroBitset        = 0;
        diskReq.key.entryPoint         = "main";
        diskReq.key.target             = "ps_5_0";
        diskReq.metadata.effectId      = L"DiskPersistTest";
        diskReq.metadata.version       = 1;
        diskReq.hlslSource             = canonical;

        auto d1 = diskCache.GetOrCompile(diskReq);
        TEST("BytecodeCache_DiskCompileReady", d1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskCompileNotFromCache", !d1.fromCache);

        // Verify a .cso file landed under the expected hierarchy.
        std::wstring expectedDir = diskRoot + L"\\DiskPersistTest\\1";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((expectedDir + L"\\*.cso").c_str(), &fd);
        bool csoExists = (h != INVALID_HANDLE_VALUE);
        if (csoExists) ::FindClose(h);
        TEST("BytecodeCache_DiskFileWritten", csoExists);

        // Clear the in-memory cache; subsequent GetOrCompile must
        // hydrate from disk, not recompile.
        diskCache.Clear();
        size_t inlineBefore = diskCache.GetStats().inlineCompiles;
        auto d2 = diskCache.GetOrCompile(diskReq);
        size_t inlineAfter = diskCache.GetStats().inlineCompiles;
        TEST("BytecodeCache_DiskRehydrateReady", d2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskRehydrateFromCache", d2.fromCache);
        TEST("BytecodeCache_DiskRehydrateNoNewCompile", inlineAfter == inlineBefore);
        TEST("BytecodeCache_DiskRehydrateBytecodeMatches",
             d2.bytecode.size() == d1.bytecode.size() &&
             memcmp(d2.bytecode.data(), d1.bytecode.data(), d1.bytecode.size()) == 0);

        // Reaper: ClearDisk wipes everything.
        auto reapStats = diskCache.ClearDisk();
        TEST("BytecodeCache_ClearDiskFreedAtLeastOne", reapStats.filesDeleted >= 1);
    }

    // An urgent request for a compile already queued moves it to the front.
    SL_TEST_CASE(TestUrgentCompileJumpsQueue, Tag::Fast)
    {
        printf("\n=== Urgent compile request jumps the queue ===\n");
        using namespace ShaderLab::Effects;
        const auto* descriptor = ShaderLabEffects::Instance().FindByName(L"ICtCp Round-Trip Validator");
        if (!descriptor) { TEST("UrgentCompile_DescriptorFound", false); return; }
        const auto node = ShaderLabEffects::CreateNode(*descriptor);
        const std::string baseSource = CanonicalizeHlslSource(node.customEffect->hlslSource);
        const std::string stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto makeRequest = [&](int index) {
            BytecodeCompileRequest request;
            request.hlslSource = baseSource + "\n// queue order test " + stamp + " " + std::to_string(index) + "\n";
            request.key.sourceHash         = HashCanonicalSource(request.hlslSource);
            request.key.paramSignatureHash = HashParamSignature({});
            request.key.includeLibraryHash = IncludeLibraryHash();
            request.key.macroBitset        = 0;
            request.key.entryPoint         = "main";
            request.key.target             = "ps_5_0";
            request.metadata.effectId = L"Queue Order Test";
            request.metadata.version  = 1;
            return request;
        };
        auto& cache = BytecodeCache::Instance();
        const int requestCount = 24;
        BytecodeCompileKey lastKey;
        for (int index = 0; index < requestCount; ++index)
        {
            auto request = makeRequest(index);
            lastKey = request.key;
            cache.RequestCompile(std::move(request));
        }
        const auto before = cache.QueuePosition(lastKey);
        cache.RequestCompile(makeRequest(requestCount - 1), /*urgent*/ true);
        const auto after = cache.QueuePosition(lastKey);
        printf("  [info] queue position before %d, after %d (-1 = taken by a worker)\n",
               before ? static_cast<int>(*before) : -1, after ? static_cast<int>(*after) : -1);
        TEST("UrgentCompile_QueuedRequestMovesToFront", before.has_value() && *before > 0 && (!after.has_value() || *after == 0));
    }

    // Workers start with the first queued job, a disk reap runs as a queued
    // job and reports its counts, and Shutdown joins every worker while
    // compiles keep working inline.
    SL_TEST_CASE(TestBytecodeCacheLifecycle, Tag::Fast)
    {
        printf("\n=== BytecodeCache worker lifecycle ===\n");
        using namespace ShaderLab::Effects;
        BytecodeCache cache;
        TEST("CacheLifecycle_NoWorkersAtConstruction", cache.WorkerCount() == 0);

        // A compile fixture only; never rasterised.
        const std::string canonical = CanonicalizeHlslSource(std::string(R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)) * 0.5; }
)"));
        auto makeRequest = [&](uint32_t salt)
        {
            BytecodeCompileRequest request;
            request.key.sourceHash         = HashCanonicalSource(canonical) ^ salt;
            request.key.paramSignatureHash = HashParamSignature({});
            request.key.includeLibraryHash = IncludeLibraryHash();
            request.key.entryPoint         = "main";
            request.key.target             = "ps_5_0";
            request.metadata.effectId      = L"CacheLifecycleTest";
            request.metadata.version       = 1;
            request.hlslSource             = canonical;
            return request;
        };

        const auto queued = makeRequest(1);
        cache.RequestCompile(queued);
        TEST("CacheLifecycle_WorkersStartWithFirstJob", cache.WorkerCount() > 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (cache.GetStatus(queued.key) == BytecodeStatus::Pending && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        TEST("CacheLifecycle_WorkerCompiled", cache.GetStatus(queued.key) == BytecodeStatus::Ready);

        // One stale entry in the cache's on-disk layout.
        const ScopedTempDir rootDir(L"ShaderLabCacheLifecycleTest");
        const auto root = rootDir.Path();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / L"Effect" / L"1", ec);
        const auto stale = root / L"Effect" / L"1" / L"0000000000000001.cso";
        { std::ofstream(stale, std::ios::binary) << "x"; }
        HANDLE file = ::CreateFileW(stale.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            FILETIME old{};
            SYSTEMTIME year2001{ 2001, 1, 0, 1, 0, 0, 0, 0 };
            ::SystemTimeToFileTime(&year2001, &old);
            ::SetFileTime(file, nullptr, &old, &old);
            ::CloseHandle(file);
        }
        cache.SetDiskCacheRoot(root.wstring());
        cache.QueueReapDisk(24ull * 60 * 60);
        std::optional<BytecodeCache::ReapResult> reaped;
        while (!(reaped = cache.LastReapResult()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        TEST("CacheLifecycle_QueuedReapReports", reaped.has_value() && reaped->filesDeleted == 1);
        TEST("CacheLifecycle_QueuedReapDeleted", !std::filesystem::exists(stale));

        cache.SetDiskCacheRoot({});
        cache.Shutdown();
        TEST("CacheLifecycle_ShutdownJoinsWorkers", cache.WorkerCount() == 0);
        const auto afterShutdown = makeRequest(2);
        cache.RequestCompile(afterShutdown);
        TEST("CacheLifecycle_NoQueueAfterShutdown",
             cache.GetStatus(afterShutdown.key) == BytecodeStatus::NotRequested && cache.WorkerCount() == 0);
        TEST("CacheLifecycle_InlineCompileAfterShutdown",
             cache.GetOrCompile(afterShutdown, 0).status == BytecodeStatus::Ready);
        std::filesystem::remove_all(root, ec);

        // Shutdown while a queued key compiles inline: the workers are busy
        // with earlier jobs, so the key is still in the queue when Shutdown
        // discards it, and the inline compile must still complete its entry.
        {
            BytecodeCache racing;
            const std::string heavy = CanonicalizeHlslSource(std::string(R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 c = Source.Load(int3(uv0.xy, 0));
    [unroll] for (int i = 0; i < 384; ++i) c = sin(c * 1.0001 + i) * cos(c.yzwx);
    return c;
}
)"));
            auto makeHeavy = [&](uint32_t salt)
            {
                BytecodeCompileRequest request = makeRequest(salt);
                request.key.sourceHash = HashCanonicalSource(heavy) ^ salt;
                request.hlslSource     = heavy;
                return request;
            };
            for (uint32_t salt = 100; salt < 116; ++salt)
                racing.RequestCompile(makeHeavy(salt));
            const auto target = makeHeavy(200);
            racing.RequestCompile(target);

            BytecodeStatus inlineStatus = BytecodeStatus::NotRequested;
            const auto inlineStart = std::chrono::steady_clock::now();
            std::thread inlineThread([&] { inlineStatus = racing.GetOrCompile(target, 0).status; });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            racing.Shutdown();
            inlineThread.join();
            const double inlineMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - inlineStart).count();
            printf("  [info] inline compile + shutdown: %.1f ms\n", inlineMs);
            TEST("CacheLifecycle_ShutdownDuringInlineCompile_Ready", inlineStatus == BytecodeStatus::Ready);
            TEST("CacheLifecycle_ShutdownDuringInlineCompile_EntryKept",
                 racing.GetStatus(target.key) == BytecodeStatus::Ready);
        }
    }
}
