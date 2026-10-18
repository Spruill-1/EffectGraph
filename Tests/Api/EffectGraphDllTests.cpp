#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraph.h"
#include "luminance_statistics.effect.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

// EffectGraph.dll as a program outside ShaderLab uses it: loaded beside the
// test runner's build, with only its C exports and COM interfaces. The DLL
// holds its own copy of the engine, so it knows only the effects registered
// through it.
namespace
{
    using namespace ShaderLab::Tests;

    using CreateContextFn = HRESULT(WINAPI*)(ID3D11Device*, const EFFECTGRAPH_CONTEXT_DESC*, REFIID, void**);
    using RegisterEffectsFn = HRESULT(WINAPI*)(const EFFECTGRAPH_EFFECT_SOURCE*);

    // <Platform>\<Config>\EffectGraph\EffectGraph.dll, beside this runner's folder.
    std::filesystem::path DllPath()
    {
        wchar_t buffer[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path().parent_path() / L"EffectGraph" / L"EffectGraph.dll";
    }

    SL_TEST_CASE(TestEffectGraphDll, Tag::Gpu)
    {
        printf("\n=== EffectGraph.dll as an outside program loads it ===\n");
        const auto path = DllPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            SkipMissingFixture(L"EffectGraph.dll (build EffectGraph.vcxproj)");
            return;
        }
        // Not freed: the DLL's worker threads may outlive the case.
        HMODULE dll = ::LoadLibraryW(path.c_str());
        TEST("Dll_Loads", dll != nullptr);
        if (!dll) return;
        auto createContext = reinterpret_cast<CreateContextFn>(::GetProcAddress(dll, "EffectGraphCreateContext"));
        auto registerEffects = reinterpret_cast<RegisterEffectsFn>(::GetProcAddress(dll, "EffectGraphRegisterEffects"));
        TEST("Dll_Exports", createContext && registerEffects && ::GetProcAddress(dll, "EffectGraphShutdown") &&
             !::GetProcAddress(dll, "ShaderLab_GetAbiVersion"));
        if (!createContext || !registerEffects) return;

        winrt::com_ptr<IEffectGraphContext> context;
        TEST("Dll_CreatesContext", SUCCEEDED(createContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put()))));
        if (!context) return;

        // Its own registry: a built-in exists once its header is registered with it.
        winrt::com_ptr<IEffectGraphNode> stats;
        TEST("Dll_NotIncludedBeforeRegistering",
             context->CreateEffectById(CLSID_EffectGraphLuminanceStatistics, stats.put()) == EFFECTGRAPH_E_EFFECT_NOT_INCLUDED);
        TEST("Dll_RegistersHeader", registerEffects(&cLuminanceStatisticsEffect) == S_OK);
        TEST("Dll_CreatesBuiltIn", SUCCEEDED(context->CreateEffectById(CLSID_EffectGraphLuminanceStatistics, stats.put())) && stats);
        if (!stats) return;

        // A 4x4 FP16 texture at 3.0 (240 nits) through statistics.
        std::vector<uint16_t> texels(4 * 4 * 4, 0x4200);   // 3.0 in half
        for (size_t i = 3; i < texels.size(); i += 4) texels[i] = 0x3C00;   // alpha 1
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 4; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ texels.data(), 4 * 8, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, &init, texture.put()));
        winrt::com_ptr<IEffectGraphNode> input;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        stats->SetInput(0, input.get());
        winrt::com_ptr<IEffectGraphValues> values;
        float peak = -1.0f;
        const bool realized = SUCCEEDED(context->Realize(stats.get(), nullptr));
        if (SUCCEEDED(stats->GetValues(values.put()))) values->GetFloat(L"Max", &peak);
        TEST("Dll_Renders", realized && std::abs(peak - 240.0f) < 0.05f);
        printf("  [info] Max through the DLL: %.3f nits\n", peak);
    }
}
