#include "pch_engine.h"
#include "TestSupport.h"
#include "ShaderTestBench.h"
#include "Rendering/DeviceStack.h"
#include "Rendering/SourceRuntime.h"
#include "Effects/EffectRegistry.h"
#include "Engine/Utf8.h"

#include <memory>

namespace ShaderLab::Tests
{
    winrt::com_ptr<ID3D11Device5> g_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext4> g_d3dContext;
    winrt::com_ptr<ID2D1Factory7> g_d2dFactory;
    winrt::com_ptr<ID2D1DeviceContext5> g_dc;
    bool g_useWarp = false;

    namespace
    {
        winrt::com_ptr<IEffectGraphContextInternal> s_caseContext;
        std::unique_ptr<Rendering::GraphEvaluator> s_caseEvaluator;
        std::unique_ptr<ShaderTestBench> s_bench;
    }

    winrt::com_ptr<IEffectGraphContextInternal> CreateTestContext()
    {
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr,
            __uuidof(IEffectGraphContextInternal), context.put_void()));
        // The process defaults the cases set, as ShaderLab's hosts pin their own.
        context->Evaluator().FollowDefaultOptions();
        return context;
    }

    IEffectGraphContextInternal& CaseContext()
    {
        if (!s_caseContext) BeginCaseContext();
        return *s_caseContext;
    }

    Rendering::GraphEvaluator& CaseEvaluator()
    {
        return s_caseEvaluator ? *s_caseEvaluator : CaseContext().Evaluator();
    }

    void ResetCaseEvaluator() { s_caseEvaluator = std::make_unique<Rendering::GraphEvaluator>(); }

    void BeginCaseContext()
    {
        EndCaseContext();
        s_caseContext = CreateTestContext();
        g_dc.copy_from(s_caseContext->DeviceContext());
        g_d2dFactory = s_caseContext->Devices().d2dFactory;
    }

    void EndCaseContext()
    {
        s_caseEvaluator.reset();
        g_dc = nullptr;
        g_d2dFactory = nullptr;
        s_caseContext = nullptr;
    }

    ShaderTestBench& Bench()
    {
        if (!s_bench)
        {
            s_bench = std::make_unique<ShaderTestBench>();
            s_bench->Initialize(g_d3dDevice.get(), g_d3dContext.get());
        }
        return *s_bench;
    }

    void ShutdownBench()
    {
        if (!s_bench) return;
        s_bench->Shutdown();
        s_bench.reset();
    }

    void PrepareAllSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory)
    {
        Rendering::PrepareSources(graph, factory, { g_dc.get(), g_d3dDevice.get(), g_d3dContext.get() },
                                  Rendering::SourceScope::All);
    }

    void PrepareSourcesAndEvaluate(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory)
    {
        graph.MarkAllDirty();
        PrepareAllSources(graph, factory);
        CaseEvaluator().Evaluate(graph, g_dc.get());
        CaseEvaluator().Evaluate(graph, g_dc.get()); // second pass for new effects
    }

    Rendering::FrameResult RunFrame(Graph::GraphModel& graph, Rendering::SecondPass secondPass)
    {
        Rendering::FrameRequest request;
        request.secondPass = secondPass;
        return Rendering::FrameExecutor::Run(graph, CaseEvaluator(), g_dc.get(), request);
    }

    Rendering::FrameResult RenderFrame(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory)
    {
        graph.MarkAllDirty();
        PrepareAllSources(graph, factory);
        return RunFrame(graph);
    }

    bool HasOutput(const Graph::EffectNode& node)
    {
        return node.cachedOutput != nullptr;
    }

    float AnalysisField(const Graph::EffectNode* node, const wchar_t* name)
    {
        if (!node) return -1.0f;
        for (const auto& field : node->analysisOutput.fields)
            if (field.name == name) return field.components[0];
        return -1.0f;
    }

    Effects::ShaderLabEffectDescriptor PatternEffect(const wchar_t* name, const std::string& body)
    {
        Effects::ShaderLabEffectDescriptor descriptor;
        descriptor.name = name;
        descriptor.effectId = name;
        descriptor.effectVersion = 1;
        descriptor.shaderType = Graph::CustomShaderType::PixelShader;
        descriptor.inputNames = { L"Source" };
        descriptor.hlslSource =
            "Texture2D Source : register(t0);\n"
            "SamplerState InputSampler : register(s0);\n"
            "float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,\n"
            "            float4 uv0 : TEXCOORD0) : SV_TARGET\n"
            "{\n"
            "    float4 input = Source.Sample(InputSampler, uv0.xy);\n"
            "    float2 pixel = floor(scenePos.xy);\n"
            + body +
            "}\n";
        return descriptor;
    }

    uint32_t AddPatternSource(Graph::GraphModel& graph, Effects::SourceNodeFactory& sourceFactory,
                              const Effects::ShaderLabEffectDescriptor& pattern, float width, float height)
    {
        auto& d2dRegistry = Effects::EffectRegistry::Instance();
        const auto floodId = graph.AddNode(Effects::SourceNodeFactory::CreateFloodSourceNode(
            winrt::Windows::Foundation::Numerics::float4{ 1.0f, 1.0f, 1.0f, 1.0f }));
        const auto cropId = graph.AddNode(d2dRegistry.CreateNode(*d2dRegistry.FindByName(L"Crop")));
        const auto patternId = graph.AddNode(Effects::ShaderLabEffects::CreateNode(pattern));
        graph.FindNode(cropId)->properties[L"Rect"] =
            winrt::Windows::Foundation::Numerics::float4{ 0.0f, 0.0f, width, height };
        graph.Connect(floodId, 0, cropId, 0);
        graph.Connect(cropId, 0, patternId, 0);
        sourceFactory.PrepareSourceNode(*graph.FindNode(floodId), g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        return patternId;
    }

    std::wstring FindFixture(const wchar_t* name)
    {
        wchar_t buffer[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(buffer).parent_path();
        for (int i = 0; i < 6 && !dir.empty(); ++i)
        {
            const auto candidate = dir / L"Tests" / L"fixtures" / name;
            if (std::filesystem::exists(candidate)) return candidate.wstring();
            dir = dir.parent_path();
        }
        return {};
    }

    void SkipMissingFixture(const wchar_t* name)
    {
        const std::string narrow = WideToUtf8(name);
        if (std::wstring_view(name).ends_with(L".mp4"))
            ReportSkip(narrow + " not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1");
        else
            ReportSkip("Tests\\fixtures\\" + narrow + " not found");
    }

    ScopedTempDir::ScopedTempDir(const wchar_t* prefix)
        : m_path(std::filesystem::temp_directory_path() /
                 (std::wstring(prefix) + L"-" + std::to_wstring(::GetCurrentProcessId())))
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }

    ScopedTempDir::~ScopedTempDir()
    {
        // Best-effort cleanup: a file still open elsewhere only leaves a
        // directory in %TEMP% behind, which the next run with this PID clears.
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
}
