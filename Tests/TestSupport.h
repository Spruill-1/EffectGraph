#pragma once

// The shared device, the per-case EffectGraph context and the helpers that
// several test files use. main() creates the device, and a fresh context on it
// for each registered case.

#include "pch_engine.h"
#include "TestCommon.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/FrameExecutor.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Engine/Api/EffectGraphInternal.h"

#include <filesystem>
#include <string>

namespace ShaderLab::Tests
{
    class ShaderTestBench;

    // The D3D11 device every case renders on.
    extern winrt::com_ptr<ID3D11Device5> g_d3dDevice;
    extern winrt::com_ptr<ID3D11DeviceContext4> g_d3dContext;
    // The running case's context's D2D factory and device context.
    extern winrt::com_ptr<ID2D1Factory7> g_d2dFactory;
    extern winrt::com_ptr<ID2D1DeviceContext5> g_dc;
    extern bool g_useWarp;

    // The EffectGraph context of the running case, created fresh for each one
    // on the shared device, as a host owns one: g_dc and g_d2dFactory are its,
    // and CaseEvaluator() is its evaluator. So every case renders on a
    // context's D2D device, with the effects a context registers, and no case
    // inherits another's effect cache, pinned options, readback interest or
    // D2D resources. A node's cachedOutput is valid only while the case runs.
    IEffectGraphContextInternal& CaseContext();
    Rendering::GraphEvaluator& CaseEvaluator();
    // A fresh evaluator for the rest of the case, used in place of the
    // context's, on the same device context.
    void ResetCaseEvaluator();
    void BeginCaseContext();
    void EndCaseContext();

    // A new EffectGraph context on the shared device, as a host would own.
    winrt::com_ptr<IEffectGraphContextInternal> CreateTestContext();

    // The HLSL math bench on the shared device, initialized on first use.
    ShaderTestBench& Bench();
    void ShutdownBench();

    // Every source prepared on the shared device.
    void PrepareAllSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory);

    // Marks the graph dirty, prepares every source and runs two Evaluate
    // passes with no draw session, so compute nodes stay queued.
    void PrepareSourcesAndEvaluate(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory);

    // One FrameExecutor frame over the whole graph, as headless renders.
    Rendering::FrameResult RunFrame(Graph::GraphModel& graph,
                                    Rendering::SecondPass secondPass = Rendering::SecondPass::IfDirty);

    // Every source re-prepared with the whole graph dirty, then one frame.
    Rendering::FrameResult RenderFrame(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory);

    bool HasOutput(const Graph::EffectNode& node);

    // The first component of a node's analysis field, or -1 when absent.
    float AnalysisField(const Graph::EffectNode* node, const wchar_t* name);

    // A one-input pixel shader whose colour is `body`, which sees `pixel`
    // (integer scene position) and `input` (the sampled Source).
    Effects::ShaderLabEffectDescriptor PatternEffect(const wchar_t* name, const std::string& body);

    // Flood -> Crop(width x height) -> pattern; returns the pattern node.
    // The factory owns the Flood's output, so it must outlive the graph.
    uint32_t AddPatternSource(Graph::GraphModel& graph, Effects::SourceNodeFactory& sourceFactory,
                              const Effects::ShaderLabEffectDescriptor& pattern, float width, float height);

    // Tests\fixtures\<name>, found by walking up from the test executable
    // (<Platform>\<Config>\ShaderLabTests\ShaderLabTests.exe). Empty if absent.
    std::wstring FindFixture(const wchar_t* name);

    // Reports a skip for a fixture that is not there, naming what makes it.
    void SkipMissingFixture(const wchar_t* name);

    // <temp>\<prefix>-<pid>: a directory of this run's own, so two runs at
    // once do not share it. Created empty; removed with everything in it
    // when the object goes out of scope.
    class ScopedTempDir
    {
    public:
        explicit ScopedTempDir(const wchar_t* prefix);
        ~ScopedTempDir();
        ScopedTempDir(const ScopedTempDir&) = delete;
        ScopedTempDir& operator=(const ScopedTempDir&) = delete;

        const std::filesystem::path& Path() const { return m_path; }

    private:
        std::filesystem::path m_path;
    };
}
