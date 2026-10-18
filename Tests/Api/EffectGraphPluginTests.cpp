#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraphPlugin.h"
#include "Effects/BytecodeCache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// EffectGraph plugins: TestEffectPlugin.dll (Tests/TestPlugin) holds one
// effect, Test Plugin Gain (colour times Gain, default 3), compiled by
// EffectGraphCompile and registered through the host interface. The build
// copies it to Effects\ beside this runner.
namespace
{
    using namespace ShaderLab::Tests;

    constexpr UINT cWidth = 16, cHeight = 8;
    constexpr const wchar_t* cEffectName = L"Test Plugin Gain";

    std::filesystem::path RunnerDir()
    {
        wchar_t buffer[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path();
    }

    std::filesystem::path PluginPath() { return RunnerDir() / L"Effects" / L"TestEffectPlugin.dll"; }

    uint16_t ToHalf(float f)
    {
        uint32_t b; std::memcpy(&b, &f, 4);
        if ((b & 0x7FFFFFFF) == 0) return static_cast<uint16_t>((b >> 16) & 0x8000);
        return static_cast<uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xFF) - 112) << 10) | ((b >> 13) & 0x3FF));
    }

    float FromHalf(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits = exponent == 0 ? sign : sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f; std::memcpy(&f, &bits, 4);
        return f;
    }

    winrt::com_ptr<ID3D11Texture2D> MakeTexture(UINT bind, const void* texels)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cWidth; desc.Height = cHeight; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init{ texels, cWidth * 8, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, texels ? &init : nullptr, texture.put()));
        return texture;
    }

    // Red of pixel (x, 0) after rendering `node` into `target`.
    float RedAfterRender(IEffectGraphContext* context, IEffectGraphNode* node, ID3D11Texture2D* target, UINT x)
    {
        if (FAILED(context->RealizeTo(node, target))) return -1.0f;
        winrt::com_ptr<ID3D11DeviceContext> immediate;
        g_d3dDevice->GetImmediateContext(immediate.put());
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, staging.put()));
        immediate->CopyResource(staging.get(), target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(immediate->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        uint16_t red;
        std::memcpy(&red, static_cast<const uint8_t*>(mapped.pData) + x * 8, 2);
        immediate->Unmap(staging.get(), 0);
        return FromHalf(red);
    }

    // Grey 0.25 on the left half, 0.5 on the right.
    std::vector<uint16_t> Texels()
    {
        std::vector<uint16_t> texels(cWidth * cHeight * 4);
        for (UINT i = 0; i < cWidth * cHeight; ++i)
        {
            const float grey = (i % cWidth) < cWidth / 2 ? 0.25f : 0.5f;
            texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = ToHalf(grey);
            texels[i * 4 + 3] = ToHalf(1.0f);
        }
        return texels;
    }

    size_t CompileCount()
    {
        const auto stats = ShaderLab::Effects::BytecodeCache::Instance().GetStats();
        return stats.inlineCompiles + stats.workerCompiles;
    }

    // Renders the plugin's effect on the grey input with `context`: Gain 3 by
    // default, then 2.
    void CheckPluginEffect(IEffectGraphContext* context, const char* prefix)
    {
        const auto texels = Texels();
        auto source = MakeTexture(D3D11_BIND_SHADER_RESOURCE, texels.data());
        auto target = MakeTexture(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr);
        winrt::com_ptr<IEffectGraphNode> input, gain;
        winrt::check_hresult(context->CreateTextureInput(source.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        const std::string created = std::string(prefix) + "_CreatesPluginEffect";
        TEST(created.c_str(), SUCCEEDED(context->CreateEffect(cEffectName, gain.put())) && gain);
        if (!gain) return;
        gain->SetInput(0, input.get());

        const float left = RedAfterRender(context, gain.get(), target.get(), 2);
        const float right = RedAfterRender(context, gain.get(), target.get(), cWidth - 2);
        printf("  [info] %s: Gain 3 renders %.4f, %.4f\n", prefix, left, right);
        const std::string byDefault = std::string(prefix) + "_DefaultGain";
        TEST(byDefault.c_str(), left == 0.75f && right == 1.5f);

        const std::string set = std::string(prefix) + "_GainProperty";
        TEST(set.c_str(), SUCCEEDED(gain->SetValueByName(L"Gain", 2.0f)) &&
             RedAfterRender(context, gain.get(), target.get(), 2) == 0.5f &&
             RedAfterRender(context, gain.get(), target.get(), cWidth - 2) == 1.0f);
    }

    // Tagged Last: registering appends to the process-wide effect list.
    SL_TEST_CASE(TestEffectGraphPlugin, Tag::Gpu | Tag::Last)
    {
        printf("\n=== EffectGraph plugins ===\n");
        const auto plugin = PluginPath();
        std::error_code ec;
        if (!std::filesystem::exists(plugin, ec))
        {
            SkipMissingFixture(L"Effects\\TestEffectPlugin.dll (build TestEffectPlugin.vcxproj)");
            return;
        }

        TEST("Plugin_MissingFileFails",
             EffectGraphLoadPlugin((RunnerDir() / L"Effects" / L"NoSuchPlugin.dll").c_str()) == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        wchar_t system[MAX_PATH]{};
        ::GetSystemDirectoryW(system, MAX_PATH);
        TEST("Plugin_NonPluginDllRefused",
             EffectGraphLoadPlugin((std::filesystem::path(system) / L"version.dll").c_str()) == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));

        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        winrt::com_ptr<IEffectGraphNode> before;
        TEST("Plugin_UnknownBeforeLoading", context->CreateEffect(cEffectName, before.put()) == EFFECTGRAPH_E_UNKNOWN_EFFECT);

        TEST("Plugin_Loads", EffectGraphLoadPlugin(plugin.c_str()) == S_OK);
        TEST("Plugin_SecondLoadIsNoOp", EffectGraphLoadPlugin(plugin.c_str()) == S_FALSE);
        // The same DLL by a relative path: found, and the same module.
        const auto relative = std::filesystem::relative(plugin, std::filesystem::current_path(), ec);
        TEST("Plugin_RelativePath", !ec && relative.is_relative() && EffectGraphLoadPlugin(relative.c_str()) == S_FALSE);

        // Served from the plugin's compiled builds: nothing is compiled.
        const size_t compiles = CompileCount();
        CheckPluginEffect(context.get(), "Plugin");
        TEST("Plugin_NoRuntimeCompile", CompileCount() == compiles);
    }

    // The same plugin, loaded by EffectGraph.dll into its own engine: a plugin
    // works with any EffectGraph host, not only ShaderLab.
    SL_TEST_CASE(TestEffectGraphPluginInDll, Tag::Gpu)
    {
        printf("\n=== EffectGraph plugins: through EffectGraph.dll ===\n");
        const auto dllPath = RunnerDir().parent_path() / L"EffectGraph" / L"EffectGraph.dll";
        const auto plugin = PluginPath();
        std::error_code ec;
        if (!std::filesystem::exists(dllPath, ec) || !std::filesystem::exists(plugin, ec))
        {
            SkipMissingFixture(L"EffectGraph.dll and Effects\\TestEffectPlugin.dll");
            return;
        }
        // Not freed: the DLL's worker threads may outlive the case.
        HMODULE dll = ::LoadLibraryW(dllPath.c_str());
        using CreateContextFn = HRESULT(WINAPI*)(ID3D11Device*, const EFFECTGRAPH_CONTEXT_DESC*, REFIID, void**);
        using LoadPluginFn = HRESULT(WINAPI*)(PCWSTR);
        auto createContext = dll ? reinterpret_cast<CreateContextFn>(::GetProcAddress(dll, "EffectGraphCreateContext")) : nullptr;
        auto loadPlugin = dll ? reinterpret_cast<LoadPluginFn>(::GetProcAddress(dll, "EffectGraphLoadPlugin")) : nullptr;
        TEST("PluginDll_Exports", createContext && loadPlugin);
        if (!createContext || !loadPlugin) return;

        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(createContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        winrt::com_ptr<IEffectGraphNode> before;
        TEST("PluginDll_UnknownBeforeLoading", context->CreateEffect(cEffectName, before.put()) == EFFECTGRAPH_E_UNKNOWN_EFFECT);
        TEST("PluginDll_Loads", loadPlugin(plugin.c_str()) == S_OK);
        CheckPluginEffect(context.get(), "PluginDll");
    }
}
