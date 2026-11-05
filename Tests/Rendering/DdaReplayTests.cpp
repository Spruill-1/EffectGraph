#include "pch_engine.h"
#include "../TestSupport.h"
#include "DdaStageSim.h"
#include "Effects/Performance.h"
#include "Graph/GraphEdits.h"
#include "Rendering/D3D11ComputeRunner.h"
#include "Rendering/DisplayProfile.h"
#include "Rendering/MathExpression.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// A desktop-duplication tonemap stage, replayed in process: the stage's
// context configuration (readback skip on, async readback off, no async or
// runtime compile) with the HDR Screenshot Tonemap from the plugin the runner
// loaded (--plugin <PrivateEffects.dll>). Without it these cases do not run.
//
// The DDA graph: texture input -> Luminance Statistics -> HDR Screenshot
// Tonemap (8bpc), with Working Space giving ClipNits and SdrWhiteNits, and
// SourcePeakNits <- Max and WhiteErrorBudget <- WhiteErrorBudget, so every
// bound value is served on the GPU and nothing needs the statistics on the CPU.
// The graphs are the plugin repository's own files (graphs/dda_screenshot.json
// and the adaptive_screenshot.json it replaces), found by walking up from the
// plugin, both from one folder; a missing file is a fixture skip, and the
// case builds that graph through the API instead.
namespace
{
    using namespace ShaderLab::Tests;
    using namespace ShaderLab;

    constexpr const wchar_t* cTonemapName = L"HDR Screenshot Tonemap (8bpc)";
    // The profile's SDR white: no default or saved value (the tone map's 200,
    // the statistics' 203, Working Space's saved 80), so only a resolved
    // Working Space binding can give it.
    constexpr float cSdrWhiteNits = 240.0f;
    // The tone map's default SdrWhiteNits and the value node 4 saves.
    constexpr float cSavedSdrWhiteNits = 200.0f;
    // Luminance Statistics counts a pixel clipped above ClipNits * this.
    constexpr float cClipMargin = 1.01f;

    uint16_t ToHalf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 112;
        if (exponent <= 0) return static_cast<uint16_t>(sign);   // below the half normals: zero
        if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | ((bits >> 13) & 0x3FFu));
    }

    // Normal halves and zero only: all ToHalf writes.
    float FromHalf(uint16_t half)
    {
        const uint32_t exponent = (half >> 10) & 0x1Fu;
        if (exponent == 0) return 0.0f;
        const uint32_t bits = (static_cast<uint32_t>(half & 0x8000u) << 16) | ((exponent + 112u) << 23) |
                              (static_cast<uint32_t>(half & 0x3FFu) << 13);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // What one frame of the replay shows: a horizontal grey ramp of 1 to 400
    // nits, and a square highlight in the top-left quarter whose side and
    // level change from frame to frame, so Max and the clipped fraction (and
    // with it WhiteErrorBudget) move.
    struct Scene
    {
        uint32_t patchSide;
        float patchNits;
    };

    class HdrDesktop
    {
    public:
        HdrDesktop(uint32_t width, uint32_t height) : m_width(width), m_height(height)
        {
            m_texels.resize(static_cast<size_t>(width) * height * 4);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                    Put(x, y, RampNits(x));
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ m_texels.data(), width * 8, 0 };
            winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, &init, m_texture.put()));
        }

        ID3D11Texture2D* Texture() const { return m_texture.get(); }
        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        // The largest patch: the region every Show rewrites.
        uint32_t MaxPatch() const { return m_width / 4; }
        uint32_t PatchOrigin() const { return m_width / 16; }

        // Writes the scene into the texture, as a desktop present would.
        void Show(const Scene& scene)
        {
            const uint32_t origin = PatchOrigin(), extent = MaxPatch();
            for (uint32_t y = origin; y < origin + extent; ++y)
                for (uint32_t x = origin; x < origin + extent; ++x)
                {
                    const bool inPatch = x - origin < scene.patchSide && y - origin < scene.patchSide;
                    Put(x, y, inPatch ? scene.patchNits : RampNits(x));
                }
            const D3D11_BOX box{ origin, origin, 0, origin + extent, origin + extent, 1 };
            const uint8_t* first = reinterpret_cast<const uint8_t*>(m_texels.data()) +
                                   (static_cast<size_t>(origin) * m_width + origin) * 8;
            g_d3dContext->UpdateSubresource(m_texture.get(), 0, &box, first, m_width * 8, 0);
        }

        float RampNits(uint32_t x) const { return 1.0f + 399.0f * static_cast<float>(x) / static_cast<float>(m_width - 1); }

        // CPU mirror of Luminance Statistics' ClippedFraction over what the
        // texture holds: the fraction of pixels whose luminance in nits is
        // above `thresholdNits`.
        double FractionAbove(float thresholdNits) const
        {
            size_t above = 0;
            for (size_t i = 0; i < m_texels.size(); i += 4)
            {
                const float r = FromHalf(m_texels[i]), g = FromHalf(m_texels[i + 1]), b = FromHalf(m_texels[i + 2]);
                if ((r * 0.2126f + g * 0.7152f + b * 0.0722f) * 80.0f > thresholdNits) ++above;
            }
            return static_cast<double>(above) / static_cast<double>(m_texels.size() / 4);
        }

    private:
        void Put(uint32_t x, uint32_t y, float nits)
        {
            const uint16_t grey = ToHalf(nits / 80.0f);
            uint16_t* texel = &m_texels[(static_cast<size_t>(y) * m_width + x) * 4];
            texel[0] = texel[1] = texel[2] = grey;
            texel[3] = ToHalf(1.0f);
        }

        uint32_t m_width, m_height;
        std::vector<uint16_t> m_texels;
        winrt::com_ptr<ID3D11Texture2D> m_texture;
    };

    winrt::com_ptr<ID3D11Texture2D> MakeSrgbTarget(uint32_t width, uint32_t height)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> target;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, target.put()));
        return target;
    }

    // The target's bytes, tightly packed (test-side readback, never timed).
    std::vector<uint8_t> ReadTarget(ID3D11Texture2D* target)
    {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, staging.put()));
        g_d3dContext->CopyResource(staging.get(), target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint8_t> bytes(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(&bytes[static_cast<size_t>(y) * desc.Width * 4],
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(desc.Width) * 4);
        g_d3dContext->Unmap(staging.get(), 0);
        return bytes;
    }

    // Waits until the GPU has finished everything submitted, so each timed
    // frame starts on an idle GPU, as a present-paced stage does.
    void WaitForGpuIdle()
    {
        D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
        winrt::com_ptr<ID3D11Query> query;
        winrt::check_hresult(g_d3dDevice->CreateQuery(&desc, query.put()));
        g_d3dContext->End(query.get());
        BOOL done = FALSE;
        while (g_d3dContext->GetData(query.get(), &done, sizeof(done), 0) == S_FALSE)
            std::this_thread::yield();
    }

    // The stage's context: no flags (no async compile, no runtime compile),
    // so the COM defaults hold: readback skip on, async readback off.
    winrt::com_ptr<IEffectGraphContextInternal> CreateStageContext()
    {
        EFFECTGRAPH_CONTEXT_DESC desc{ EFFECTGRAPH_CONTEXT_FLAGS_NONE, nullptr };
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), &desc,
            __uuidof(IEffectGraphContextInternal), context.put_void()));
        auto profile = Rendering::PresetP3_1000();
        profile.caps.sdrWhiteLevelNits = cSdrWhiteNits;
        context->SetDisplayProfile(profile);
        return context;
    }

    struct DdaGraph
    {
        winrt::com_ptr<IEffectGraphNode> input, workingSpace, stats, tonemap, root;
        uint32_t statsId{ 0 }, tonemapId{ 0 };
        bool built{ false };
    };

    // The plugin repository's graphs/ folder: the nearest one walking up from
    // the plugin DLL that holds `name`. Empty when there is none.
    std::filesystem::path FindPluginGraphs(const wchar_t* name)
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(g_pluginPath).parent_path();
        for (int level = 0; level < 8 && !dir.empty(); ++level)
        {
            const auto graphs = dir / L"graphs";
            if (std::filesystem::is_regular_file(graphs / name, ec)) return graphs;
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        return {};
    }

    bool BoundTo(const Graph::EffectNode* node, const wchar_t* property, uint32_t sourceId, const wchar_t* field)
    {
        if (!node) return false;
        const auto it = node->propertyBindings.find(property);
        if (it == node->propertyBindings.end() || it->second.wholeArray || it->second.sources.size() != 1 ||
            !it->second.sources[0].has_value())
            return false;
        const auto& source = *it->second.sources[0];
        return source.sourceNodeId == sourceId && source.sourceFieldName == field;
    }

    bool SavedFloatIs(const Graph::EffectNode* node, const wchar_t* property, float expected)
    {
        if (!node) return false;
        const auto it = node->properties.find(property);
        if (it == node->properties.end()) return false;
        const float* value = std::get_if<float>(&it->second);
        return value && *value == expected;
    }

    // A node property's current value through the public API, with any
    // binding resolved. NaN when it cannot be read.
    float ValueOf(IEffectGraphNode* node, const wchar_t* property)
    {
        float value = std::nanf("");
        if (FAILED(node->GetValueByName(property, D2D1_PROPERTY_TYPE_FLOAT, reinterpret_cast<BYTE*>(&value),
                                        sizeof(value))))
            return std::nanf("");
        return value;
    }

    // The DDA graph through the public API, as the stage builds it. With
    // `numericExpression`, the graph before the fix instead: WhiteErrorBudget
    // <- Numeric Expression min(A*2, 20) <- ClippedFraction, a CPU consumer
    // of the statistics.
    DdaGraph BuildDdaGraph(IEffectGraphContextInternal& context, ID3D11Texture2D* source, bool numericExpression,
                           const char* prefix)
    {
        DdaGraph g;
        winrt::com_ptr<IEffectGraphNode> expression;
        bool ok = SUCCEEDED(context.CreateTextureInput(source, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, g.input.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Working Space", g.workingSpace.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Luminance Statistics", g.stats.put())) &&
                  SUCCEEDED(context.CreateEffect(cTonemapName, g.tonemap.put()));
        const std::string created = std::string(prefix) + "_GraphCreated";
        TEST(created.c_str(), ok);
        if (!ok) return g;

        ok = SUCCEEDED(g.stats->SetInput(0, g.input.get())) &&
             SUCCEEDED(g.tonemap->SetInput(0, g.input.get())) &&
             SUCCEEDED(g.stats->Bind(L"ClipNits", g.workingSpace.get(), L"SdrWhiteNits")) &&
             SUCCEEDED(g.tonemap->Bind(L"SdrWhiteNits", g.workingSpace.get(), L"SdrWhiteNits")) &&
             SUCCEEDED(g.tonemap->Bind(L"SourcePeakNits", g.stats.get(), L"Max"));
        if (ok && numericExpression)
        {
            ok = SUCCEEDED(context.CreateEffect(L"Numeric Expression", expression.put())) &&
                 Graph::SetNodeProperty(context.Model(), context.NodeId(expression.get()), L"Expression",
                                        std::wstring(L"min(A*2,20)")) &&
                 SUCCEEDED(expression->Bind(L"A", g.stats.get(), L"ClippedFraction")) &&
                 SUCCEEDED(g.tonemap->Bind(L"WhiteErrorBudget", expression.get(), L"Result"));
        }
        else if (ok)
        {
            ok = SUCCEEDED(g.tonemap->Bind(L"WhiteErrorBudget", g.stats.get(), L"WhiteErrorBudget"));
        }
        const std::string wired = std::string(prefix) + "_GraphWired";
        TEST(wired.c_str(), ok);
        g.statsId = context.NodeId(g.stats.get());
        g.tonemapId = context.NodeId(g.tonemap.get());
        g.root = g.tonemap;
        g.built = ok;
        return g;
    }

    // A saved graph, loaded the way a host loads one, with the host's texture
    // wired to input 0 of the statistics (node 2) and the tone map (node 4).
    // `dda`: the fixed graph, realized at node 4, whose bindings must be the
    // fix's; otherwise the graph before it, realized at its White Level
    // Adjustment (node 5), with WhiteErrorBudget <- Numeric Expression (node 3).
    DdaGraph LoadDdaGraph(IEffectGraphContextInternal& context, const std::filesystem::path& file,
                          ID3D11Texture2D* source, bool dda, const char* prefix)
    {
        DdaGraph g;
        bool loaded = false;
        try
        {
            std::ifstream stream(file, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
            context.ReplaceGraph(Graph::GraphModel::FromJson(winrt::to_hstring(bytes)));
            loaded = true;
        }
        catch (const winrt::hresult_error& e)
        {
            printf("  [info] loading %ls failed: 0x%08X %ls\n", file.c_str(), static_cast<uint32_t>(e.code()),
                   e.message().c_str());
        }
        catch (const std::exception& e)
        {
            printf("  [info] loading %ls failed: %s\n", file.c_str(), e.what());
        }
        const std::string loadedName = std::string(prefix) + "_FileLoaded";
        TEST(loadedName.c_str(), loaded);
        if (!loaded) return g;

        const uint32_t rootId = dda ? 4u : 5u;
        const bool wiredOk =
            SUCCEEDED(context.CreateTextureInput(source, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, g.input.put())) &&
            context.NodeFor(1, g.workingSpace.put()) == S_OK &&
            context.NodeFor(2, g.stats.put()) == S_OK &&
            context.NodeFor(4, g.tonemap.put()) == S_OK &&
            context.NodeFor(rootId, g.root.put()) == S_OK &&
            SUCCEEDED(g.stats->SetInput(0, g.input.get())) &&
            SUCCEEDED(g.tonemap->SetInput(0, g.input.get()));
        const std::string wired = std::string(prefix) + "_FileWired";
        TEST(wired.c_str(), wiredOk);
        if (!wiredOk) return g;

        // What the file must say for the measurement to be about it.
        const auto& model = context.Model();
        const auto* stats = model.FindNode(2);
        const auto* tonemap = model.FindNode(4);
        const bool shape = stats && stats->name == L"Luminance Statistics" && tonemap && tonemap->name == cTonemapName &&
                           BoundTo(stats, L"ClipNits", 1, L"SdrWhiteNits") &&
                           BoundTo(tonemap, L"SdrWhiteNits", 1, L"SdrWhiteNits") &&
                           BoundTo(tonemap, L"SourcePeakNits", 2, L"Max") &&
                           (dda ? BoundTo(tonemap, L"WhiteErrorBudget", 2, L"WhiteErrorBudget") &&
                                      // min(2 x ClippedFraction, 20), the expression's formula
                                      SavedFloatIs(stats, L"BudgetScale", 2.0f) &&
                                      SavedFloatIs(stats, L"BudgetMax", 20.0f) &&
                                      model.Nodes().size() == 4   // nodes 1, 2, 4 and the texture input
                                : BoundTo(tonemap, L"WhiteErrorBudget", 3, L"Result"));
        const std::string shapeName = std::string(prefix) + (dda ? "_FileBindsTheFix" : "_FileIsTheGraphBeforeTheFix");
        TEST(shapeName.c_str(), shape);
        // The DDA file runs the catalog's statistics: the cell reduction.
        if (dda)
        {
            const auto* current = Effects::ShaderLabEffects::Instance().FindByName(L"Luminance Statistics");
            const std::string currentName = std::string(prefix) + "_FileStatisticsAtCatalogVersion";
            TEST(currentName.c_str(), stats && current && stats->customEffect &&
                 stats->customEffect->shaderLabEffectVersion == current->effectVersion &&
                 stats->customEffect->dispatchShape == Graph::DispatchShape::ReduceCells);
        }
        g.statsId = 2;
        g.tonemapId = 4;
        g.built = shape;
        return g;
    }

    // Copies queued on the GPU before a present, so a blocking readback in it
    // has a pipeline to drain, as it does under a desktop's load. Sized at
    // construction to take about `targetMs` of GPU time.
    class GpuLoad
    {
    public:
        GpuLoad(uint32_t width, uint32_t height, double targetMs)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, m_a.put()));
            winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, m_b.put()));
            // The first copies into fresh textures pay for their allocation.
            Measure();
            for (m_copies = 4; m_copies < 1024; m_copies *= 2)
            {
                m_ms = (std::min)(Measure(), Measure());
                if (m_ms >= targetMs) break;
            }
        }

        void Queue()
        {
            for (uint32_t i = 0; i < m_copies; ++i)
                g_d3dContext->CopyResource(i % 2 ? m_a.get() : m_b.get(), i % 2 ? m_b.get() : m_a.get());
            g_d3dContext->Flush();
        }

        uint32_t Copies() const { return m_copies; }
        double Ms() const { return m_ms; }

    private:
        double Measure()
        {
            WaitForGpuIdle();
            const auto start = std::chrono::steady_clock::now();
            Queue();
            WaitForGpuIdle();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        winrt::com_ptr<ID3D11Texture2D> m_a, m_b;
        uint32_t m_copies{ 4 };
        double m_ms{ 0.0 };
    };

    // A value no statistics field takes: written over Max before each
    // present, so Max after it shows whether this present read back.
    constexpr float cPoison = -12345.0f;

    // One present: RealizeTo timed on the CPU, and what the statistics did.
    struct Present
    {
        HRESULT hr{ S_OK };
        double cpuMs{ 0.0 };
        // Dispatches whose readback the evaluator skipped.
        uint64_t skipped{ 0 };
        // A blocking readback ran: Max reached the CPU with this scene's peak.
        bool readBack{ false };
        // Any statistics field on the CPU after the present.
        bool fieldsOnCpu{ false };
        // The statistics node carries a runtime error after the present.
        bool statsFailed{ false };
        // The statistics dispatched and succeeded: exactly one skipped
        // readback or one blocking readback. The skip counter moves before
        // the bridge's result is checked, so a failed dispatch counts too;
        // the node's runtime error tells the two apart.
        bool Dispatched() const { return !statsFailed && ((skipped == 1 && !readBack) || (skipped == 0 && readBack)); }
    };

    Present RunPresent(IEffectGraphContextInternal& context, const DdaGraph& g, HdrDesktop& desktop,
                       const Scene& scene, ID3D11Texture2D* target, GpuLoad* load)
    {
        desktop.Show(scene);
        winrt::com_ptr<IEffectGraphInput> input = g.input.as<IEffectGraphInput>();
        input->Invalidate();
        if (g.statsId)
            if (auto* stats = context.Model().FindNode(g.statsId))
                for (auto& field : stats->analysisOutput.fields)
                    if (field.name == L"Max") field.components[0] = cPoison;
        WaitForGpuIdle();
        if (load) load->Queue();

        Present present;
        const uint64_t skippedBefore = Performance::SkippedCpuReadbacks();
        const auto start = std::chrono::steady_clock::now();
        present.hr = context.RealizeTo(g.root.get(), target);
        const auto end = std::chrono::steady_clock::now();
        present.cpuMs = std::chrono::duration<double, std::milli>(end - start).count();
        present.skipped = Performance::SkippedCpuReadbacks() - skippedBefore;
        if (g.statsId)
            if (const auto* stats = context.Model().FindNode(g.statsId))
            {
                const float max = AnalysisField(stats, L"Max");
                present.readBack = std::fabs(max - scene.patchNits) <= 0.01f * scene.patchNits;
                present.fieldsOnCpu = !stats->analysisOutput.fields.empty();
                present.statsFailed = !stats->runtimeError.empty();
                if (present.statsFailed)
                    printf("  [info] statistics runtime error: %ls\n", stats->runtimeError.c_str());
            }
        return present;
    }

    Scene SceneFor(uint32_t frame, const HdrDesktop& desktop)
    {
        constexpr float cLevels[] = { 1000.0f, 1600.0f, 2500.0f, 4000.0f };
        const uint32_t step = desktop.MaxPatch() / 4;
        return { step * (1 + frame % 4), cLevels[(frame / 2) % 4] };
    }

    struct ArmResult
    {
        uint32_t presents{ 0 }, failed{ 0 }, dispatched{ 0 }, readbacks{ 0 }, fieldsOnCpu{ 0 }, statsFailed{ 0 };
        std::vector<double> timedMs;
        double Median() const
        {
            if (timedMs.empty()) return 0.0;
            auto sorted = timedMs;
            std::sort(sorted.begin(), sorted.end());
            return sorted[sorted.size() / 2];
        }
        double Mean() const
        {
            double sum = 0.0;
            for (double ms : timedMs) sum += ms;
            return timedMs.empty() ? 0.0 : sum / static_cast<double>(timedMs.size());
        }
        // Dispatched and read back, blocking, on every present.
        bool ReadBackEveryPresent() const
        {
            return presents > 0 && failed == 0 && statsFailed == 0 && dispatched == presents && readbacks == presents;
        }
    };

    // `warmup` untimed presents, then `timed` timed ones; every present
    // changes the scene, so the statistics dispatch on every one.
    ArmResult RunArm(IEffectGraphContextInternal& context, const DdaGraph& g, HdrDesktop& desktop,
                     ID3D11Texture2D* target, uint32_t warmup, uint32_t timed, GpuLoad* load)
    {
        ArmResult arm;
        for (uint32_t i = 0; i < warmup + timed; ++i)
        {
            const Present present = RunPresent(context, g, desktop, SceneFor(i, desktop), target, load);
            ++arm.presents;
            if (FAILED(present.hr)) ++arm.failed;
            if (present.Dispatched()) ++arm.dispatched;
            if (present.readBack) ++arm.readbacks;
            if (present.fieldsOnCpu) ++arm.fieldsOnCpu;
            if (present.statsFailed) ++arm.statsFailed;
            if (i >= warmup) arm.timedMs.push_back(present.cpuMs);
        }
        return arm;
    }

    void PrintArm(const char* name, const ArmResult& arm)
    {
        std::printf("  [info] %-38s %u presents, %u failed, %u statistics errors, %u dispatched, "
                    "%u blocking readback(s), %u left fields on the CPU; RealizeTo CPU median %.3f ms, "
                    "mean %.3f ms over %zu timed\n",
                    name, arm.presents, arm.failed, arm.statsFailed, arm.dispatched, arm.readbacks, arm.fieldsOnCpu,
                    arm.Median(), arm.Mean(), arm.timedMs.size());
    }

    // Two bits per GPU-bindable parameter in declaration order
    // (SourcePeakNits, WhiteErrorBudget): 2 = the analysis texture (lane 3).
    constexpr uint32_t cBothOnAnalysisTexture = 2u | (2u << 2);

    float FieldOf(IEffectGraphNode* node, const wchar_t* name)
    {
        winrt::com_ptr<IEffectGraphValues> values;
        float value = -1.0f;
        if (SUCCEEDED(node->GetValues(values.put())) && values)
            values->GetFloat(name, &value);
        return value;
    }

    size_t BytesDiffering(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        if (a.size() != b.size()) return (std::max)(a.size(), b.size());
        size_t differing = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++differing;
        return differing;
    }

    // Stage 0 of the dirty-rects work: the DDA graph never reads the
    // statistics back, and its frame CPU is the no-readback figure. The
    // graph before the fix (a Numeric Expression bound to ClippedFraction)
    // and the same graph with the readback skip off are the controls: each
    // present must be seen to read back (Max poisoned before it, this scene's
    // peak after), or the counters measure nothing. Every arm also runs with
    // copies queued on the GPU, where a blocking readback pays the drain.
    SL_TEST_CASE(Stage0_NoBlockingReadback, Tag::Gpu)
    {
        printf("\n=== Stage 0: DDA graph, no blocking statistics readback ===\n");
        if (g_pluginPath.empty())
        {
            ReportNotRun("Stage0_NoBlockingReadback: needs --plugin <PrivateEffects.dll>");
            return;
        }

        // 2880x1800 is decision #76's 5.2 Mpx; WARP gets a smaller frame.
        const uint32_t width = g_useWarp ? 640 : 2880;
        const uint32_t height = g_useWarp ? 400 : 1800;
        const uint32_t warmup = g_useWarp ? 2 : 5;
        const uint32_t timed = g_useWarp ? 6 : 40;
        printf("  [info] %ux%u (%.2f Mpx), %u warm-up + %u timed presents per arm, GPU idle before each\n",
               width, height, width * height / 1.0e6, warmup, timed);

        HdrDesktop desktop(width, height);
        auto target = MakeSrgbTarget(width, height);
        GpuLoad load(width, height, 4.0);
        printf("  [info] loaded arms queue %u copies before each present: %.3f ms of GPU work\n",
               load.Copies(), load.Ms());

        // Both files from one graphs folder, each tested when present. A
        // missing file is a missing fixture (--require-fixtures fails it):
        // the deliverable is the file, and the API-built stand-in that runs
        // in its place does not test it.
        const auto graphsDir = FindPluginGraphs(L"dda_screenshot.json");
        const auto ddaFile = graphsDir.empty() ? graphsDir : graphsDir / L"dda_screenshot.json";
        std::error_code ec;
        const auto beforeFile = graphsDir.empty() || !std::filesystem::is_regular_file(
                                                         graphsDir / L"adaptive_screenshot.json", ec)
                                    ? std::filesystem::path{}
                                    : graphsDir / L"adaptive_screenshot.json";
        if (ddaFile.empty())
            ReportSkip("Stage0_NoBlockingReadback: no graphs/dda_screenshot.json above the plugin; "
                       "the API-built DDA graph runs instead");
        else
            printf("  [info] DDA graph: %ls\n", ddaFile.c_str());
        if (beforeFile.empty())
            ReportSkip("Stage0_NoBlockingReadback: no adaptive_screenshot.json beside dda_screenshot.json; "
                       "the API-built graph before the fix runs instead");
        else
            printf("  [info] graph before the fix: %ls\n", beforeFile.c_str());

        auto stage = CreateStageContext();
        const auto stageOptions = stage->Evaluator().Options();
        TEST("Stage0_ContextSkipsReadback", stageOptions.skipUnneededCpuReadback && !stageOptions.asyncAnalysisReadback &&
                                            !stageOptions.compilePrebuiltEffects);
        DdaGraph dda = !ddaFile.empty() ? LoadDdaGraph(*stage, ddaFile, desktop.Texture(), true, "Stage0_Dda")
                                        : BuildDdaGraph(*stage, desktop.Texture(), false, "Stage0_Dda");
        if (!dda.built) return;

        // ---- arm A: the DDA graph, as the stage runs it ----
        const ArmResult fixedArm = RunArm(*stage, dda, desktop, target.get(), warmup, timed, nullptr);
        const ArmResult fixedLoaded = RunArm(*stage, dda, desktop, target.get(), warmup, timed, &load);
        PrintArm("DDA graph (skip on):", fixedArm);
        PrintArm("DDA graph (skip on), GPU loaded:", fixedLoaded);
        const uint32_t gpuBits = stage->Evaluator().AppliedGpuBits(dda.tonemapId);
        printf("  [info] tone map GPU-binding bits %u (want %u)\n", gpuBits, cBothOnAnalysisTexture);
        TEST("Stage0_EveryPresentRealized", fixedArm.failed == 0 && fixedLoaded.failed == 0);
        TEST("Stage0_StatisticsNeverFail", fixedArm.statsFailed == 0 && fixedLoaded.statsFailed == 0);
        // A skipped readback is counted at the bridge call, once per dispatch;
        // a dispatch that failed is not counted (Present::Dispatched).
        TEST("Stage0_StatisticsDispatchEveryPresent",
             fixedArm.dispatched == fixedArm.presents && fixedLoaded.dispatched == fixedLoaded.presents);
        TEST("Stage0_NoBlockingReadbackOnAnyPresent", fixedArm.readbacks == 0 && fixedLoaded.readbacks == 0);
        // Late, async and stale-field reads all unpack into the node: none did.
        TEST("Stage0_StatisticsNeverReachTheCpu", fixedArm.fieldsOnCpu == 0 && fixedLoaded.fieldsOnCpu == 0);
        TEST("Stage0_BothBindingsOnTheGpu", gpuBits == cBothOnAnalysisTexture);

        // ---- the bound values are live and are what the tone map renders ----
        // Two scenes far apart; each realized, then the statistics read on
        // purpose and the same tone map rendered with them as plain values
        // (SdrWhiteNits too, the profile's), which must match byte for byte.
        // Then each value is changed alone in the reference -- the budget
        // halved, the peak halved, SDR white at the saved 200 -- and each
        // change must show, or the match could not see that value wrong.
        winrt::com_ptr<IEffectGraphNode> reference;
        const bool made = SUCCEEDED(stage->CreateEffect(cTonemapName, reference.put())) &&
                          SUCCEEDED(reference->SetInput(0, dda.input.get()));
        TEST("Stage0_ReferenceToneMapCreated", made);
        auto referenceTarget = MakeSrgbTarget(width, height);
        const Scene scenes[] = { { desktop.MaxPatch() / 4, 1000.0f }, { desktop.MaxPatch(), 4000.0f } };
        std::vector<uint8_t> rendered[2];
        float peak[2]{}, budget[2]{}, clipped[2]{};
        double cpuClipped[2]{};
        float workingWhite[2]{}, clipNits[2]{}, toneMapWhite[2]{};
        size_t mismatched[2]{}, budgetShows[2]{}, peakShows[2]{}, whiteShows[2]{};
        // The reference rendered with (peak, budget, white), against the bound render.
        const auto differsBy = [&](int s, float peakNits, float budgetValue, float whiteNits) {
            reference->SetValueByName(L"SourcePeakNits", peakNits);
            reference->SetValueByName(L"WhiteErrorBudget", budgetValue);
            reference->SetValueByName(L"SdrWhiteNits", whiteNits);
            const HRESULT hr = stage->RealizeTo(reference.get(), referenceTarget.get());
            if (FAILED(hr)) printf("  [info] reference RealizeTo failed 0x%08X\n", static_cast<uint32_t>(hr));
            return FAILED(hr) ? rendered[s].size() + 1 : BytesDiffering(ReadTarget(referenceTarget.get()), rendered[s]);
        };
        for (int s = 0; s < 2 && made; ++s)
        {
            const Present present = RunPresent(*stage, dda, desktop, scenes[s], target.get(), nullptr);
            rendered[s] = ReadTarget(target.get());
            peak[s] = FieldOf(dda.stats.get(), L"Max");
            budget[s] = FieldOf(dda.stats.get(), L"WhiteErrorBudget");
            clipped[s] = FieldOf(dda.stats.get(), L"ClippedFraction");
            cpuClipped[s] = desktop.FractionAbove(cSdrWhiteNits * cClipMargin);
            workingWhite[s] = FieldOf(dda.workingSpace.get(), L"SdrWhiteNits");
            clipNits[s] = ValueOf(dda.stats.get(), L"ClipNits");
            toneMapWhite[s] = ValueOf(dda.tonemap.get(), L"SdrWhiteNits");
            mismatched[s] = differsBy(s, peak[s], budget[s], cSdrWhiteNits);
            budgetShows[s] = differsBy(s, peak[s], budget[s] * 0.5f, cSdrWhiteNits);
            peakShows[s] = differsBy(s, peak[s] * 0.5f, budget[s], cSdrWhiteNits);
            whiteShows[s] = differsBy(s, peak[s], budget[s], cSavedSdrWhiteNits);
            printf("  [info] scene %d (patch %u px at %.0f nits, hr 0x%08X): Max %.3f, ClippedFraction %.6f "
                   "(CPU %.6f), WhiteErrorBudget %.6f; Working Space SdrWhiteNits %.1f, ClipNits %.1f, tone map "
                   "SdrWhiteNits %.1f\n",
                   s, scenes[s].patchSide, scenes[s].patchNits, static_cast<uint32_t>(present.hr), peak[s],
                   clipped[s], cpuClipped[s], budget[s], workingWhite[s], clipNits[s], toneMapWhite[s]);
            printf("  [info] scene %d bytes differing from the bound render (of %zu): plain values %zu; "
                   "budget halved %zu; peak halved %zu; SDR white %.0f %zu\n",
                   s, rendered[s].size(), mismatched[s], budgetShows[s], peakShows[s], cSavedSdrWhiteNits,
                   whiteShows[s]);
        }
        if (made)
        {
            size_t movedOutsidePatch = 0;
            for (uint32_t y = height / 2; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t at = (static_cast<size_t>(y) * width + x) * 4;
                    if (rendered[0][at] != rendered[1][at] || rendered[0][at + 1] != rendered[1][at + 1] ||
                        rendered[0][at + 2] != rendered[1][at + 2])
                        ++movedOutsidePatch;
                }
            printf("  [info] pixels in the lower half (no patch) that changed between the scenes: %zu\n",
                   movedOutsidePatch);
            TEST("Stage0_BoundValuesMove", peak[1] > peak[0] && budget[1] > budget[0] && budget[0] > 0.0f);
            // Working Space follows the profile, and both CPU-served bindings resolve to it.
            TEST("Stage0_SdrWhiteFromTheProfile",
                 workingWhite[0] == cSdrWhiteNits && workingWhite[1] == cSdrWhiteNits && clipNits[0] == cSdrWhiteNits &&
                 clipNits[1] == cSdrWhiteNits && toneMapWhite[0] == cSdrWhiteNits && toneMapWhite[1] == cSdrWhiteNits);
            // The shader counted against the bound ClipNits: at the saved 203 or
            // 200 the fraction would be about 0.1 higher. A FP16 ramp step is
            // under 0.001 of the frame.
            TEST("Stage0_ClippedFractionAtTheProfilesWhite",
                 std::fabs(clipped[0] - cpuClipped[0]) <= 0.002 && std::fabs(clipped[1] - cpuClipped[1]) <= 0.002);
            // The graph it replaces computed min(2 x ClippedFraction, 20).
            const auto expressionBudget = [](float fraction) { return (std::min)(2.0f * fraction, 20.0f); };
            TEST("Stage0_BudgetIsTheExpressionsFormula",
                 std::fabs(budget[0] - expressionBudget(clipped[0])) <= 1e-5f &&
                 std::fabs(budget[1] - expressionBudget(clipped[1])) <= 1e-5f);
            TEST("Stage0_SceneChangeReachesPixelsOutsideThePatch", movedOutsidePatch > 0);
            TEST("Stage0_RendersTheValuesItBinds", mismatched[0] == 0 && mismatched[1] == 0);
            // Without these, a value read from the wrong field or a stale frame
            // could render the same bytes.
            TEST("Stage0_BudgetReachesThePixels", budgetShows[0] > 0 && budgetShows[1] > 0);
            TEST("Stage0_PeakReachesThePixels", peakShows[0] > 0 && peakShows[1] > 0);
            TEST("Stage0_SdrWhiteReachesThePixels", whiteShows[0] > 0 && whiteShows[1] > 0);
            // The floor arm below renders the reference with the scene's values.
            reference->SetValueByName(L"SourcePeakNits", peak[1]);
            reference->SetValueByName(L"WhiteErrorBudget", budget[1]);
            reference->SetValueByName(L"SdrWhiteNits", cSdrWhiteNits);
        }

        // ---- the floor: the same frame with the tone map's values plain ----
        // The statistics still dispatch (a dirty analysis node demands
        // itself) and skip their readback; only the bindings are gone.
        ArmResult floorArm, floorLoaded;
        // The fix and the floor again, present by present with the order
        // alternating, so each pair shares a clock state: [0] idle, [1] loaded.
        std::vector<double> pairedFix[2], pairedFloor[2];
        if (made)
        {
            DdaGraph plain;
            plain.input = dda.input;
            plain.root = reference;
            plain.statsId = dda.statsId;
            floorArm = RunArm(*stage, plain, desktop, referenceTarget.get(), warmup, timed, nullptr);
            floorLoaded = RunArm(*stage, plain, desktop, referenceTarget.get(), warmup, timed, &load);
            for (int loaded = 0; loaded < 2; ++loaded)
                for (uint32_t i = 0; i < warmup + timed; ++i)
                    for (int k = 0; k < 2; ++k)
                    {
                        const bool fix = (k == 0) == (i % 2 == 0);
                        const Present present = fix
                            ? RunPresent(*stage, dda, desktop, SceneFor(i, desktop), target.get(), loaded ? &load : nullptr)
                            : RunPresent(*stage, plain, desktop, SceneFor(i, desktop), referenceTarget.get(),
                                         loaded ? &load : nullptr);
                        if (i >= warmup) (fix ? pairedFix : pairedFloor)[loaded].push_back(present.cpuMs);
                    }
            PrintArm("plain-value tone map (floor):", floorArm);
            PrintArm("plain-value tone map, GPU loaded:", floorLoaded);
            TEST("Stage0_FloorDispatchesWithoutReadback",
                 floorArm.failed == 0 && floorLoaded.failed == 0 && floorArm.dispatched == floorArm.presents &&
                 floorLoaded.dispatched == floorLoaded.presents && floorArm.readbacks == 0 && floorLoaded.readbacks == 0);
        }

        // ---- arm C: the same graph with the readback skip off ----
        auto unskipped = stageOptions;
        unskipped.skipUnneededCpuReadback = false;
        stage->Evaluator().SetOptions(unskipped);
        const ArmResult unskippedArm = RunArm(*stage, dda, desktop, target.get(), warmup, timed, nullptr);
        const ArmResult unskippedLoaded = RunArm(*stage, dda, desktop, target.get(), warmup, timed, &load);
        stage->Evaluator().SetOptions(stageOptions);
        PrintArm("DDA graph, skip off (control):", unskippedArm);
        PrintArm("DDA graph, skip off, GPU loaded:", unskippedLoaded);
        TEST("Stage0_Control_SkipOffReadsBackEveryPresent",
             unskippedArm.ReadBackEveryPresent() && unskippedLoaded.ReadBackEveryPresent());

        // ---- arm B: the graph before the fix, in a context of its own ----
        // The engine has no expression language; this one evaluates the
        // graph's expression, min(A*2, 20).
        Rendering::SetMathExpressionEvaluator([](const std::wstring& expression, const wchar_t* const* names,
                                                 const float* inputs, size_t count, float& out, std::wstring* error) {
            for (size_t i = 0; i < count; ++i)
                if (expression == L"min(A*2,20)" && std::wstring(names[i]) == L"A")
                {
                    out = (std::min)(inputs[i] * 2.0f, 20.0f);
                    return true;
                }
            if (error) *error = L"the test evaluator knows only min(A*2,20)";
            out = 0.0f;
            return false;
        });
        ArmResult beforeArm, beforeLoaded;
        {
            auto before = CreateStageContext();
            DdaGraph old = !beforeFile.empty()
                               ? LoadDdaGraph(*before, beforeFile, desktop.Texture(), false, "Stage0_Before")
                               : BuildDdaGraph(*before, desktop.Texture(), true, "Stage0_Before");
            if (old.built)
            {
                beforeArm = RunArm(*before, old, desktop, target.get(), warmup, timed, nullptr);
                beforeLoaded = RunArm(*before, old, desktop, target.get(), warmup, timed, &load);
            }
        }
        Rendering::SetMathExpressionEvaluator(nullptr);
        PrintArm("before the fix (Numeric Expression):", beforeArm);
        PrintArm("before the fix, GPU loaded:", beforeLoaded);
        TEST("Stage0_Control_ExpressionReadsBackEveryPresent",
             beforeArm.ReadBackEveryPresent() && beforeLoaded.ReadBackEveryPresent());

        // ---- frame CPU: the fix runs at the no-readback figure ----
        printf("  [info] RealizeTo CPU median, GPU idle:   DDA %.3f ms, floor %.3f ms, skip off %.3f ms, "
               "before the fix %.3f ms\n",
               fixedArm.Median(), floorArm.Median(), unskippedArm.Median(), beforeArm.Median());
        printf("  [info] RealizeTo CPU median, GPU loaded: DDA %.3f ms, floor %.3f ms, skip off %.3f ms, "
               "before the fix %.3f ms\n",
               fixedLoaded.Median(), floorLoaded.Median(), unskippedLoaded.Median(), beforeLoaded.Median());
        TIMING_TEST("Stage0_FrameCpuBelowSkipOff", fixedArm.Median() < unskippedArm.Median());
        TIMING_TEST("Stage0_FrameCpuBelowBeforeTheFix", fixedArm.Median() < beforeArm.Median());
        // Under load a blocking readback waits for the queued copies. The fix
        // must sit nearer the floor than half way to either control.
        const auto nearFloor = [&](const ArmResult& control) {
            return fixedLoaded.Median() < floorLoaded.Median() + 0.5 * (control.Median() - floorLoaded.Median());
        };
        TIMING_TEST("Stage0_LoadedFrameCpuNearTheFloorNotSkipOff", nearFloor(unskippedLoaded));
        TIMING_TEST("Stage0_LoadedFrameCpuNearTheFloorNotBeforeTheFix", nearFloor(beforeLoaded));
        // The gate itself: the fix at the floor, idle and loaded. The margin
        // is the bindings' own CPU (resolving three, choosing the GPU build),
        // which the floor's plain values do not pay; an extra stall of 0.2 ms
        // in Release is well past it.
        // Judged on the interleaved pairs: fail only when the fix is dearer
        // than 1.6 x the floor + 0.1 ms with 99% confidence.
        PrintPairedTiming("Stage0 fix - floor, GPU idle", ComparePaired(pairedFix[0], pairedFloor[0]));
        PrintPairedTiming("Stage0 fix - floor, GPU loaded", ComparePaired(pairedFix[1], pairedFloor[1]));
        const auto atFloor = [](const std::vector<double>& fix, const std::vector<double>& floor) {
            return !fix.empty() && !ComparePaired(fix, floor, 1.6, 0.1).ConfidentlyAbove();
        };
        TIMING_TEST("Stage0_FrameCpuAtTheFloor", atFloor(pairedFix[0], pairedFloor[0]));
        TIMING_TEST("Stage0_LoadedFrameCpuAtTheFloor", atFloor(pairedFix[1], pairedFloor[1]));
    }
}

// ---- Stage 1: the replay harness, pure chain --------------------------------
//
// Frames (synthetic scenarios, or --recording) -> CreateTextureInput -> a tone
// map with unbound parameters (the plugin's HDR Screenshot Tonemap with
// --plugin; without it the built-in Gamut Map, a per-pixel shader too, since
// the built-in tone maps are D3D11 compute and Whole until stage 4) ->
// RealizeToIncremental into an _SRGB intermediate -> the simulated stage's
// per-client accumulators and the copy into UNORM client surfaces
// (DdaStageSim). Moves arrive as dirty rects at their destinations until
// stage 5. Each frame is checked against a second context that renders the
// same frames with dirtyRegions off and D2D output caching off: the
// forced-whole render, rounding as the uncached incremental root does.
namespace
{
    namespace Dda = ShaderLab::Tests::Dda;

    const wchar_t* ChainEffect() { return g_pluginPath.empty() ? L"Gamut Map" : cTonemapName; }

    struct ReplayArm
    {
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::com_ptr<IEffectGraphNode> input, root;
        winrt::com_ptr<ID3D11Texture2D> target;
        bool ok{ false };
    };

    enum class ArmKind
    {
        Incremental,    // dirtyRegions on: the stage under test
        ForcedWhole,    // dirtyRegions off, nothing cached: the reference every frame is checked against
        SwitchOff,      // dirtyRegions and pointImageFilter off: 1.0's path, the cost control
    };

    ReplayArm MakeArm(ID3D11Texture2D* desktop, const wchar_t* effect, ArmKind kind)
    {
        ReplayArm arm;
        arm.context = CreateStageContext();
        auto options = arm.context->Evaluator().Options();
        options.dirtyRegions = kind == ArmKind::Incremental;
        // 1.0 read image pins through the linear filter.
        if (kind == ArmKind::SwitchOff) options.pointImageFilter = false;
        // An incremental root is drawn uncached (a cached one rounds
        // differently), so the reference renders uncached too.
        if (kind == ArmKind::ForcedWhole) options.effectOutputCaching = false;
        arm.context->Evaluator().SetOptions(options);
        D3D11_TEXTURE2D_DESC desc{};
        desktop->GetDesc(&desc);
        arm.ok = SUCCEEDED(arm.context->CreateTextureInput(desktop, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, arm.input.put())) &&
                 SUCCEEDED(arm.context->CreateEffect(effect, arm.root.put())) &&
                 SUCCEEDED(arm.root->SetInput(0, arm.input.get()));
        arm.target = MakeSrgbTarget(desc.Width, desc.Height);
        return arm;
    }

    // The control arm's frame: the desktop texture changed behind the
    // engine's back, and dirtyRegions is off, so the realize is whole.
    HRESULT RealizeWhole(ReplayArm& arm)
    {
        arm.input.as<IEffectGraphInput>()->Invalidate();
        RECT drawn[1]{};
        UINT32 count = 0;
        return arm.context.as<IEffectGraphContext2>()->RealizeToIncremental(
            arm.root.get(), arm.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, drawn, 1, &count, nullptr, 0, nullptr);
    }

    winrt::com_ptr<ID3D11Texture2D> MakeClientSurface(uint32_t width, uint32_t height)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> surface;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, surface.put()));
        return surface;
    }

    // A client on a device of its own: the stage writes a shared surface
    // under a keyed mutex (key 0 to the stage, key 1 to the client); the
    // client queues a copy into its own texture and releases at once.
    class RemoteClient
    {
    public:
        bool Create(uint32_t width, uint32_t height, std::string& error)
        {
            winrt::com_ptr<IDXGIDevice> dxgiDevice = g_d3dDevice.as<IDXGIDevice>();
            winrt::com_ptr<IDXGIAdapter> adapter;
            dxgiDevice->GetAdapter(adapter.put());
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            HRESULT hr = D3D11CreateDevice(g_useWarp ? nullptr : adapter.get(),
                                           g_useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                           m_device.put(), nullptr, m_context.put());
            if (FAILED(hr)) { error = std::format("second device: 0x{:08X}", static_cast<uint32_t>(hr)); return false; }

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            hr = g_d3dDevice->CreateTexture2D(&desc, nullptr, m_onStage.put());
            if (FAILED(hr)) { error = std::format("shared surface: 0x{:08X}", static_cast<uint32_t>(hr)); return false; }
            HANDLE handle = nullptr;
            hr = m_onStage.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                                    nullptr, &handle);
            if (FAILED(hr)) { error = std::format("shared handle: 0x{:08X}", static_cast<uint32_t>(hr)); return false; }
            hr = m_device.as<ID3D11Device1>()->OpenSharedResource1(handle, IID_PPV_ARGS(m_onClient.put()));
            CloseHandle(handle);
            if (FAILED(hr)) { error = std::format("opening the shared surface: 0x{:08X}", static_cast<uint32_t>(hr)); return false; }
            m_stageMutex = m_onStage.as<IDXGIKeyedMutex>();
            m_clientMutex = m_onClient.as<IDXGIKeyedMutex>();
            desc.MiscFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = m_device->CreateTexture2D(&desc, nullptr, m_own.put());
            if (FAILED(hr)) { error = std::format("client copy: 0x{:08X}", static_cast<uint32_t>(hr)); return false; }
            return true;
        }

        ID3D11Texture2D* StageSurface() const { return m_onStage.get(); }
        IDXGIKeyedMutex* StageMutex() const { return m_stageMutex.get(); }

        // Queues a copy of what the stage handed over and releases at once.
        void Receive()
        {
            winrt::check_hresult(m_clientMutex->AcquireSync(1, INFINITE));
            m_context->CopyResource(m_own.get(), m_onClient.get());
            winrt::check_hresult(m_clientMutex->ReleaseSync(0));
        }

        std::vector<uint8_t> Bytes() const { return Dda::ReadBytes(m_own.get()); }

    private:
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        winrt::com_ptr<ID3D11Texture2D> m_onStage, m_onClient, m_own;
        winrt::com_ptr<IDXGIKeyedMutex> m_stageMutex, m_clientMutex;
    };

    // The sRGB code a linear value encodes to.
    int SrgbCode(float linear)
    {
        const float v = (std::max)(0.0f, (std::min)(1.0f, linear));
        const float encoded = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<int>(std::lround(encoded * 255.0f));
    }

    // Assertion 4: the realized bytes against the tone map's own 8-bit grid,
    // recovered from an FP32 render of the same frame (its output is the
    // grid re-decoded to linear). Counts mismatching channels and the
    // largest code distance.
    struct EncodeFinding
    {
        bool measured{ false };
        size_t mismatches{ 0 };
        int maxDistance{ 0 };
        size_t channels{ 0 };
        // FP32 channels more than cGridTolerance codes off the 8-bit grid:
        // with any, the reference is not the tone map's grid and the
        // mismatch count means nothing.
        size_t offGrid{ 0 };
        float quantize{ std::nanf("") };
    };

    constexpr double cGridTolerance = 0.15;

    double SrgbCodeExact(double linear)
    {
        const double v = (std::max)(0.0, (std::min)(1.0, linear));
        return 255.0 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
    }

    void CheckEncode(ReplayArm& control, const std::vector<uint8_t>& realized, uint32_t width, uint32_t height,
                     EncodeFinding& finding)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> fp32;
        if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, fp32.put()))) return;
        float quantize = std::nanf("");
        if (SUCCEEDED(control.root->GetValueByName(L"Quantize", D2D1_PROPERTY_TYPE_FLOAT,
                                                   reinterpret_cast<BYTE*>(&quantize), sizeof(quantize))))
            finding.quantize = quantize;
        // Caching off, as the incremental root is drawn.
        const auto saved = control.context->Evaluator().Options();
        auto uncached = saved;
        uncached.effectOutputCaching = false;
        control.context->Evaluator().SetOptions(uncached);
        const HRESULT hr = control.context->RealizeTo(control.root.get(), fp32.get());
        control.context->Evaluator().SetOptions(saved);
        if (FAILED(hr)) return;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, staging.put()));
        g_d3dContext->CopyResource(staging.get(), fp32.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (uint32_t y = 0; y < height; ++y)
        {
            const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t* bgra = &realized[(static_cast<size_t>(y) * width + x) * 4];
                const int expected[3] = { SrgbCode(row[x * 4 + 2]), SrgbCode(row[x * 4 + 1]), SrgbCode(row[x * 4 + 0]) };
                for (int c = 0; c < 3; ++c)
                {
                    const double code = SrgbCodeExact(row[x * 4 + 2 - c]);
                    if (std::abs(code - std::round(code)) > cGridTolerance) ++finding.offGrid;
                    const int distance = std::abs(static_cast<int>(bgra[c]) - expected[c]);
                    ++finding.channels;
                    if (distance == 0) continue;
                    ++finding.mismatches;
                    finding.maxDistance = (std::max)(finding.maxDistance, distance);
                }
            }
        }
        g_d3dContext->Unmap(staging.get(), 0);
        finding.measured = true;
    }

    // One frame source: changes the desktop (and its texture) and reports
    // the frame. Start paints frame 0, before the stage's first realize.
    struct FrameSource
    {
        std::function<bool(Dda::Desktop&)> start;
        std::function<bool(Dda::Desktop&, Dda::DesktopFrame&)> next;
    };

    struct ReplayResult
    {
        bool built{ false };
        HRESULT primed{ E_FAIL };
        uint32_t presents{ 0 }, noFrames{ 0 }, failedRealizes{ 0 };
        // Assertion 1: frames (and pixels) changed outside the drawn rects.
        uint32_t clipFrames{ 0 };
        size_t clipPixels{ 0 };
        // Assertion 2: frames (and pixels) where the target differs from the
        // forced-whole render.
        uint32_t wholeFrames{ 0 };
        size_t wholePixels{ 0 };
        // Assertion 3, per client: acquires, acquires whose surface differs
        // from the forced-whole render, acquires whose metadata missed a
        // changed pixel.
        uint32_t acquires[2]{}, surfaceMismatches[2]{}, coverageMisses[2]{};
        uint32_t heldPresents{ 0 };   // most presents one held acquire accumulated
        // Instrument checks: presents whose rects drew nothing, presents
        // whose change did not reach the target.
        uint32_t emptyDraws{ 0 };
        uint32_t staticFrames{ 0 };
        double drawnPixels{ 0 }, targetPixels{ 0 };
        size_t keyframeMismatches{ 0 };
        uint32_t accumulated{ 0 };
        uint32_t maxRectsIn{ 0 };
        EncodeFinding encode;
        // The region path: presents whose rects do not cover the target (the
        // ones that must not draw it whole), presents with no metadata, and
        // presents whose drawn rects were not what changed (the changed
        // rects exactly, or inside their bounding box past 16 rects; the
        // whole target for a present with no metadata).
        uint32_t rectPresents{ 0 }, wholePresents{ 0 }, drawnMisses{ 0 };
        // The clip canary: a target pixel outside the changed rects set to a
        // sentinel before the present, checked where the realize did not
        // draw; any change there is a draw past its clip.
        uint32_t canaryChecks{ 0 }, canaryClobbered{ 0 };
    };

    // Clipped to the surface.
    RECT ClipRect(const RECT& r, uint32_t width, uint32_t height)
    {
        RECT c{ (std::max)(r.left, 0L), (std::max)(r.top, 0L), (std::min)(r.right, static_cast<LONG>(width)),
                (std::min)(r.bottom, static_cast<LONG>(height)) };
        if (c.right < c.left) c.right = c.left;
        if (c.bottom < c.top) c.bottom = c.top;
        return c;
    }

    std::vector<uint8_t> Mask(const std::vector<RECT>& rects, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0);
        for (const auto& r : rects)
        {
            const RECT c = ClipRect(r, width, height);
            if (c.right <= c.left) continue;
            for (LONG y = c.top; y < c.bottom; ++y)
                std::memset(&mask[static_cast<size_t>(y) * width + c.left], 1, static_cast<size_t>(c.right - c.left));
        }
        return mask;
    }

    // What a successful realize after a successful one must draw: exactly the
    // changed rects (the engine drops only rects inside others), within their
    // bounding box past DirtyRegion's 16, and the whole target for a present
    // with no metadata.
    bool DrawnAsExpected(const std::vector<RECT>& changed, bool whole, const std::vector<RECT>& drawn,
                         uint32_t width, uint32_t height)
    {
        const RECT all{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        const auto drawnMask = Mask(drawn, width, height);
        if (whole) return drawnMask == Mask({ all }, width, height);
        const auto changedMask = Mask(changed, width, height);
        if (changed.size() <= Graph::DirtyRegion::cMaxRects) return drawnMask == changedMask;
        RECT box{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
        for (const auto& r : changed)
            box = { (std::min)(box.left, r.left), (std::min)(box.top, r.top), (std::max)(box.right, r.right),
                    (std::max)(box.bottom, r.bottom) };
        const auto boxMask = Mask({ box }, width, height);
        for (size_t i = 0; i < drawnMask.size(); ++i)
            if ((changedMask[i] && !drawnMask[i]) || (drawnMask[i] && !boxMask[i])) return false;
        return true;
    }

    // Whether the clipped rects cover the whole surface.
    bool CoversSurface(const std::vector<RECT>& rects, uint32_t width, uint32_t height)
    {
        const auto mask = Mask(rects, width, height);
        return std::ranges::all_of(mask, [](uint8_t m) { return m != 0; });
    }

    // A point outside every changed rect: a corner or the centre.
    bool CanaryPoint(const std::vector<RECT>& changed, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
    {
        const uint32_t candidates[][2] = { { 1, 1 }, { width - 2, 1 }, { 1, height - 2 }, { width - 2, height - 2 },
                                           { width / 2, height / 2 } };
        for (const auto& c : candidates)
            if (!Dda::Inside(changed, c[0], c[1]))
            {
                x = c[0];
                y = c[1];
                return true;
            }
        return false;
    }

    void WritePixel(ID3D11Texture2D* texture, uint32_t x, uint32_t y, const uint8_t* bgra)
    {
        const D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
        g_d3dContext->UpdateSubresource(texture, 0, &box, bgra, 4, 0);
    }

    constexpr uint32_t cHoldPresents = 3;

    // How the replay's arms filter image pins. Linear is 1.0's filter: the
    // control for the filter switch, not a configuration that must pass.
    // LinearThenPoint primes with it and switches both live contexts to point.
    enum class ReplayFilter { Point, Linear, LinearThenPoint };

    ReplayResult Replay(uint32_t width, uint32_t height, const FrameSource& source, bool checkEncode,
                        ReplayFilter filter = ReplayFilter::Point)
    {
        ReplayResult result;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        if (!source.start(desktop)) return result;
        auto incremental = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::Incremental);
        auto control = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::ForcedWhole);
        if (!incremental.ok || !control.ok) return result;
        const auto setFilter = [&](bool point) {
            for (auto* arm : { &incremental, &control })
            {
                auto options = arm->context->Evaluator().Options();
                options.pointImageFilter = point;
                arm->context->Evaluator().SetOptions(options);
            }
        };
        setFilter(filter == ReplayFilter::Point);
        result.built = true;

        Dda::StageSim stage(incremental.context.get(), incremental.input.get(), incremental.root.get(),
                            incremental.target.get(), g_d3dContext.get());
        winrt::com_ptr<ID3D11Texture2D> surfaces[2] = { MakeClientSurface(width, height), MakeClientSurface(width, height) };
        stage.AddClient(surfaces[0].get());   // acquires every present
        stage.AddClient(surfaces[1].get());   // holds its frame across cHoldPresents presents
        std::vector<uint8_t> seen[2];

        result.primed = stage.Prime();
        const HRESULT primedControl = RealizeWhole(control);
        if (FAILED(result.primed) || FAILED(primedControl)) return result;
        if (filter == ReplayFilter::LinearThenPoint) setFilter(true);
        auto reference = Dda::ReadBytes(control.target.get());
        const auto acquire = [&](int client)
        {
            const auto delivery = stage.Acquire(client);
            if (FAILED(delivery.hr)) return;
            ++result.acquires[client];
            result.heldPresents = (std::max)(result.heldPresents, delivery.presents);
            const auto surface = Dda::ReadBytes(surfaces[client].get());
            if (Dda::Compare(surface, reference, width, {}).differing != 0) ++result.surfaceMismatches[client];
            // What changed since this client's last frame lies inside the
            // metadata it was handed.
            if (!seen[client].empty() && Dda::Compare(seen[client], reference, width, delivery.rects).outside != 0)
                ++result.coverageMisses[client];
            seen[client] = surface;
        };
        acquire(0);
        acquire(1);
        const auto statsBefore = incremental.context->Evaluator().RegionStats();

        Dda::DesktopFrame frame;
        while (source.next(desktop, frame))
        {
            if (frame.kind == Dda::FrameKind::NoFrame)
            {
                ++result.noFrames;   // not a present: no realize, no tick
                continue;
            }
            ++result.presents;
            if (frame.accumulatedFrames > 1) ++result.accumulated;
            const auto changed = Dda::ChangedRects(frame);
            result.maxRectsIn = (std::max)(result.maxRectsIn, static_cast<uint32_t>(changed.size()));
            const bool covering = frame.whole || CoversSurface(changed, width, height);
            if (frame.whole) ++result.wholePresents;
            else if (!covering) ++result.rectPresents;

            const auto before = Dda::ReadBytes(incremental.target.get());
            // The clip canary, written behind the engine's back.
            uint32_t canaryX = 0, canaryY = 0;
            const bool canary = !covering && CanaryPoint(changed, width, height, canaryX, canaryY);
            const size_t canaryAt = (static_cast<size_t>(canaryY) * width + canaryX) * 4;
            uint8_t sentinel[4]{};
            if (canary)
            {
                for (int c = 0; c < 4; ++c) sentinel[c] = static_cast<uint8_t>(before[canaryAt + c] ^ 0xA5);
                WritePixel(incremental.target.get(), canaryX, canaryY, sentinel);
            }
            const HRESULT hr = stage.Present(frame);
            const HRESULT hrControl = RealizeWhole(control);
            if (FAILED(hr) || FAILED(hrControl))
            {
                if (canary) WritePixel(incremental.target.get(), canaryX, canaryY, &before[canaryAt]);
                ++result.failedRealizes;
                continue;
            }
            auto after = Dda::ReadBytes(incremental.target.get());
            if (canary && !Dda::Inside(stage.LastDrawn(), canaryX, canaryY))
            {
                ++result.canaryChecks;
                if (std::memcmp(&after[canaryAt], sentinel, 4) != 0) ++result.canaryClobbered;
                WritePixel(incremental.target.get(), canaryX, canaryY, &before[canaryAt]);
                std::memcpy(&after[canaryAt], &before[canaryAt], 4);
            }
            reference = Dda::ReadBytes(control.target.get());

            const auto clip = Dda::Compare(before, after, width, stage.LastDrawn());
            if (clip.outside) { ++result.clipFrames; result.clipPixels += clip.outside; }
            const auto whole = Dda::Compare(after, reference, width, {});
            if (whole.differing) { ++result.wholeFrames; result.wholePixels += whole.differing; }
            if ((!changed.empty() || frame.whole) && stage.LastDrawn().empty()) ++result.emptyDraws;
            if (clip.differing == 0) ++result.staticFrames;
            if (!DrawnAsExpected(changed, frame.whole, stage.LastDrawn(), width, height)) ++result.drawnMisses;

            acquire(0);
            if (result.presents % cHoldPresents == 0) acquire(1);
            if (checkEncode && (result.presents % 8 == 1)) CheckEncode(control, after, width, height, result.encode);
        }
        const auto& statsAfter = incremental.context->Evaluator().RegionStats();
        result.drawnPixels = static_cast<double>(statsAfter.pixelsDrawn - statsBefore.pixelsDrawn);
        result.targetPixels = static_cast<double>(statsAfter.pixelsTarget - statsBefore.pixelsTarget);
        return result;
    }

    // The checks every scenario must pass, named by scenario.
    // `everyPresentChanges`: the scenario changes visible content on every
    // present (the synthetic ones), so a present that leaves the target as it
    // was means both arms went stale. A recording may present nothing visible.
    void ReportReplay(const std::string& name, const ReplayResult& r, bool everyPresentChanges)
    {
        printf("  [info] %s: %u presents, %u pointer-only, %u realizes failed; drawn %.4f of the target; "
               "up to %u rects in a frame; %u presents changed nothing on the target\n",
               name.c_str(), r.presents, r.noFrames, r.failedRealizes,
               r.targetPixels > 0 ? r.drawnPixels / r.targetPixels : 0.0, r.maxRectsIn, r.staticFrames);
        printf("  [info] %s: %u presents with rects, %u with no metadata; %u drew other than what changed; "
               "clip canary checked on %u presents, overwritten on %u\n", name.c_str(), r.rectPresents, r.wholePresents,
               r.drawnMisses, r.canaryChecks, r.canaryClobbered);
        if (r.clipFrames || r.wholeFrames)
            printf("  [info] %s: %u frames (%zu px) changed outside the rects, %u frames (%zu px) differ from the "
                   "forced-whole render\n", name.c_str(), r.clipFrames, r.clipPixels, r.wholeFrames, r.wholePixels);
        TEST((name + "_Built").c_str(), r.built && SUCCEEDED(r.primed));
        TEST((name + "_EveryRealizeSucceeded").c_str(), r.presents > 0 && r.failedRealizes == 0);
        TEST((name + "_1_NothingOutsideTheRects").c_str(), r.presents > 0 && r.clipFrames == 0);
        TEST((name + "_2_TargetEqualsForcedWhole").c_str(), r.presents > 0 && r.wholeFrames == 0);
        TEST((name + "_3_EveryPresentClientCurrent").c_str(),
             r.acquires[0] == r.presents + 1 && r.surfaceMismatches[0] == 0 && r.coverageMisses[0] == 0);
        TEST((name + "_3_HoldingClientCurrent").c_str(),
             r.acquires[1] >= 2 && r.surfaceMismatches[1] == 0 && r.coverageMisses[1] == 0 &&
             r.heldPresents >= cHoldPresents);
        // The instrument: rects that changed something drew something.
        TEST((name + "_LivePipeline").c_str(), r.emptyDraws == 0 &&
             (everyPresentChanges ? r.presents > 0 && r.staticFrames == 0 : r.staticFrames < r.presents));
        // The rect path ran: each present drew what changed and no more, so
        // presents with rects drew less than the target, and a draw past its
        // clip shows on the canary.
        const double share = r.targetPixels > 0 ? r.drawnPixels / r.targetPixels : 0.0;
        TEST((name + "_RegionPathDrawsWhatChanged").c_str(),
             r.presents > 0 && r.drawnMisses == 0 && (r.rectPresents == 0 || share < 1.0));
        TEST((name + "_1_ClipCanaryHolds").c_str(), r.canaryClobbered == 0 && (r.rectPresents == 0 || r.canaryChecks > 0));
        if (r.encode.measured)
        {
            if (r.encode.offGrid == 0)
                printf("  [info] %s finding (assertion 4, Quantize %.0f): %zu of %zu channels differ from the tone map's "
                       "8-bit grid, by up to %d codes; every FP32 channel lies within %.2f codes of the grid\n",
                       name.c_str(), r.encode.quantize, r.encode.mismatches, r.encode.channels, r.encode.maxDistance,
                       cGridTolerance);
            else
                printf("  [info] %s assertion 4 not measured (Quantize %.0f): %zu of %zu FP32 channels lie off the 8-bit "
                       "grid, so the reference is not the tone map's grid\n",
                       name.c_str(), r.encode.quantize, r.encode.offGrid, r.encode.channels);
        }
    }

    FrameSource SyntheticSource(Dda::Scenario& scenario)
    {
        FrameSource source;
        source.start = [&scenario](Dda::Desktop& desktop)
        {
            scenario.Start(desktop);
            desktop.UploadAll();
            return true;
        };
        source.next = [&scenario](Dda::Desktop& desktop, Dda::DesktopFrame& frame)
        {
            if (!scenario.Next(desktop, frame)) return false;
            if (frame.whole) desktop.UploadAll();
            else if (frame.kind == Dda::FrameKind::Present) desktop.Upload(Dda::ChangedRects(frame));
            return true;
        };
        return source;
    }

    constexpr Dda::ScenarioKind cScenarios[] = {
        Dda::ScenarioKind::IdleCursor, Dda::ScenarioKind::Scroll, Dda::ScenarioKind::HdrVideo,
        Dda::ScenarioKind::WindowDrag, Dda::ScenarioKind::CoverAll, Dda::ScenarioKind::VideoAndScroll,
    };

    // Wider than 2048 on hardware, so D2D tiles; small on WARP.
    void ReplaySize(uint32_t& width, uint32_t& height, uint32_t& presents)
    {
        width = g_useWarp ? 480 : 3840;
        height = g_useWarp ? 270 : 2160;
        presents = g_useWarp ? 18 : 24;
    }

    SL_TEST_CASE(Stage1_ReplaySynthetic, Tag::Gpu)
    {
        printf("\n=== Stage 1: replay, pure chain, synthetic scenarios (%ls) ===\n", ChainEffect());
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        for (const auto kind : cScenarios)
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = Replay(width, height, SyntheticSource(*scenario), !g_pluginPath.empty());
            ReportReplay(std::string("Replay_") + Dda::ScenarioName(kind), result, true);
        }
        // A small desktop as well: there a linear-filtered image pin rendered
        // a pixel differently inside a clipped rect than in a whole draw.
        for (const auto kind : { Dda::ScenarioKind::WindowDrag, Dda::ScenarioKind::VideoAndScroll })
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = Replay(256, 144, SyntheticSource(*scenario), false);
            ReportReplay(std::string("Replay_") + Dda::ScenarioName(kind) + "_256x144", result, true);
        }
        if (g_pluginPath.empty())
            ReportNotRun("Replay assertion 4 (the tone map's 8-bit grid): needs --plugin <PrivateEffects.dll>");
    }

    // Assertions 5 and 6. A root whose evaluation fails for a while, then
    // recovers: the realizes in between fail, every acquire meanwhile times
    // out, and the first good realize draws every rect the failed ones were
    // handed. The failure is the evaluator's own: derived constants that do
    // not fit the shader's variable, on the ICtCp Gamut Map.
    class FaultyHooks : public Effects::IEffectHooks
    {
    public:
        bool failing{ false };

        const Effects::ShaderLabEffectDescriptor::DerivedConstantsFn* DerivedConstants(std::wstring_view id) const override
        {
            const auto* real = Effects::BuiltInEffectHooks().DerivedConstants(id);
            if (!real) return nullptr;
            m_real = real;
            return &m_wrapped;
        }

        const Effects::ShaderLabEffectDescriptor::ImageOutputSizeFn* ImageOutputSize(std::wstring_view id) const override
        {
            return Effects::BuiltInEffectHooks().ImageOutputSize(id);
        }

    private:
        mutable const Effects::ShaderLabEffectDescriptor::DerivedConstantsFn* m_real{ nullptr };
        Effects::ShaderLabEffectDescriptor::DerivedConstantsFn m_wrapped =
            [this](const std::map<std::wstring, Graph::PropertyValue>& props,
                   const Effects::ShaderLabEffectDescriptor::DerivedConstantWriter& write)
            {
                (*m_real)(props, [&](std::wstring_view name, const float* data, size_t count)
                {
                    // One float too many: "does not fit". The writer copies
                    // no more than the variable holds.
                    write(name, data, failing ? count + 1 : count);
                });
            };
    };

    SL_TEST_CASE(Stage1_ReplayFailedRealize, Tag::Gpu)
    {
        printf("\n=== Stage 1: a failed realize, then recovery ===\n");
        constexpr uint32_t cWidth = 320, cHeight = 180;
        constexpr const wchar_t* cRoot = L"ICtCp Gamut Map";
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        auto scenario = Dda::MakeScenario(Dda::ScenarioKind::WindowDrag, 64);
        scenario->Start(desktop);
        desktop.UploadAll();
        auto incremental = MakeArm(desktop.Texture(), cRoot, ArmKind::Incremental);
        auto control = MakeArm(desktop.Texture(), cRoot, ArmKind::ForcedWhole);
        TEST("Failed_Built", incremental.ok && control.ok);
        if (!incremental.ok || !control.ok) return;
        FaultyHooks hooks;
        incremental.context->Evaluator().SetEffectHooks(&hooks);
        struct RestoreHooks
        {
            IEffectGraphContextInternal* context;
            ~RestoreHooks() { context->Evaluator().SetEffectHooks(&Effects::BuiltInEffectHooks()); }
        } restore{ incremental.context.get() };

        Dda::StageSim stage(incremental.context.get(), incremental.input.get(), incremental.root.get(),
                            incremental.target.get(), g_d3dContext.get());
        auto surface = MakeClientSurface(cWidth, cHeight);
        stage.AddClient(surface.get());
        const bool primed = SUCCEEDED(stage.Prime()) && SUCCEEDED(RealizeWhole(control));
        stage.Acquire(0);
        const auto present = [&](std::vector<RECT>* changed)
        {
            Dda::DesktopFrame frame;
            scenario->Next(desktop, frame);
            desktop.Upload(Dda::ChangedRects(frame));
            if (changed)
                for (const auto& r : Dda::ChangedRects(frame)) changed->push_back(r);
            const HRESULT hr = stage.Present(frame);
            RealizeWhole(control);
            return hr;
        };
        // Two good presents, then three failing ones.
        bool good = primed && SUCCEEDED(present(nullptr)) && SUCCEEDED(present(nullptr));
        stage.Acquire(0);
        hooks.failing = true;
        std::vector<RECT> missed;
        HRESULT failures[3];
        for (auto& hr : failures) hr = present(&missed);
        const bool allFailed = std::ranges::all_of(failures, [](HRESULT hr) { return hr == EFFECTGRAPH_E_NODE_FAILED; });
        TEST("Failed_FailureInjected", good && allFailed);
        // Assertion 6: no surface while the last realize failed.
        const auto timedOut = stage.Acquire(0);
        TEST("Failed_6_AcquireTimesOut", timedOut.hr == DXGI_ERROR_WAIT_TIMEOUT);
        TEST("Failed_TickDue", stage.TickDue());

        // Recovery on a present: the earlier rects are drawn with its own.
        hooks.failing = false;
        const HRESULT recovered = present(&missed);
        const auto drawn = stage.LastDrawn();
        size_t uncovered = 0;
        for (const auto& r : missed)
            for (LONG y = (std::max)(r.top, 0L); y < (std::min)(r.bottom, static_cast<LONG>(cHeight)); ++y)
                for (LONG x = (std::max)(r.left, 0L); x < (std::min)(r.right, static_cast<LONG>(cWidth)); ++x)
                    if (!Dda::Inside(drawn, x, y)) ++uncovered;
        const auto target = Dda::ReadBytes(incremental.target.get());
        const auto reference = Dda::ReadBytes(control.target.get());
        printf("  [info] recovery drew %zu rects; %zu pixels of the failed presents' rects outside them\n",
               drawn.size(), uncovered);
        TEST("Failed_5_RecoveryRedrawsEarlierRects", recovered == S_OK && uncovered == 0);
        TEST("Failed_5_RecoveredTargetEqualsForcedWhole", Dda::Compare(target, reference, cWidth, {}).differing == 0);
        // Assertion 6: then a current surface.
        const auto delivered = stage.Acquire(0);
        const auto surfaceBytes = Dda::ReadBytes(surface.get());
        TEST("Failed_6_ThenDeliversForcedWhole", delivered.hr == S_OK &&
                                                 Dda::Compare(surfaceBytes, reference, cWidth, {}).differing == 0);

        // The retry tick: a failure with no present after it is retried
        // without new rects, and draws the failed present's rects.
        hooks.failing = true;
        std::vector<RECT> tickMissed;
        const HRESULT failedAgain = present(&tickMissed);
        hooks.failing = false;
        const bool due = stage.TickDue();
        const HRESULT ticked = due ? stage.Tick() : E_FAIL;
        size_t tickUncovered = 0;
        for (const auto& r : tickMissed)
            for (LONG y = (std::max)(r.top, 0L); y < (std::min)(r.bottom, static_cast<LONG>(cHeight)); ++y)
                for (LONG x = (std::max)(r.left, 0L); x < (std::min)(r.right, static_cast<LONG>(cWidth)); ++x)
                    if (!Dda::Inside(stage.LastDrawn(), x, y)) ++tickUncovered;
        TEST("Failed_TickRetriesAndDraws", failedAgain == EFFECTGRAPH_E_NODE_FAILED && due && ticked == S_OK &&
                                           tickUncovered == 0 && !stage.TickDue());
        const auto afterTick = stage.Acquire(0);
        TEST("Failed_TickedSurfaceEqualsForcedWhole",
             afterTick.hr == S_OK && Dda::Compare(Dda::ReadBytes(surface.get()), Dda::ReadBytes(control.target.get()),
                                                  cWidth, {}).differing == 0);

        // A realize without the flag draws whole: it clears the target, then
        // fails. Only the frame's rects are pending, so the next incremental
        // realize must draw whole, not just those rects over a cleared target.
        hooks.failing = true;
        Dda::DesktopFrame flaglessFrame;
        scenario->Next(desktop, flaglessFrame);
        const auto flaglessRects = Dda::ChangedRects(flaglessFrame);
        desktop.Upload(flaglessRects);
        incremental.input.as<IEffectGraphInput2>()->InvalidateRects(flaglessRects.data(),
                                                                   static_cast<UINT32>(flaglessRects.size()));
        RECT flaglessDrawn[16]{};
        UINT32 flaglessCount = 0;
        const HRESULT flaglessFailed = incremental.context.as<IEffectGraphContext2>()->RealizeToIncremental(
            incremental.root.get(), incremental.target.get(), EFFECTGRAPH_REALIZE_FLAGS_NONE, flaglessDrawn, 16,
            &flaglessCount, nullptr, 0, nullptr);
        RealizeWhole(control);
        hooks.failing = false;
        const HRESULT afterFlagless = stage.Tick();
        const auto& redrawn = stage.LastDrawn();
        const bool redrewWhole = redrawn.size() == 1 && redrawn[0].left == 0 && redrawn[0].top == 0 &&
                                 redrawn[0].right == static_cast<LONG>(cWidth) && redrawn[0].bottom == static_cast<LONG>(cHeight);
        const auto flaglessDiff = Dda::Compare(Dda::ReadBytes(incremental.target.get()),
                                               Dda::ReadBytes(control.target.get()), cWidth, {});
        printf("  [info] after a failed realize without the flag: the next drew %zu rect(s)%s; %zu pixels differ "
               "from the forced-whole render\n", redrawn.size(), redrewWhole ? " (whole)" : "", flaglessDiff.differing);
        TEST("Failed_FlaglessFailureThenDrawsWhole",
             flaglessFailed == EFFECTGRAPH_E_NODE_FAILED && afterFlagless == S_OK && redrewWhole);
        TEST("Failed_FlaglessRecoveryEqualsForcedWhole", afterFlagless == S_OK && flaglessDiff.differing == 0);
    }

    // Assertion 5's other half: a root waiting on its shader compile. The
    // realizes while it compiles return EFFECTGRAPH_E_COMPILE_PENDING, every
    // acquire meanwhile times out, and the first realize after the compile
    // draws every rect handed in while it waited.
    SL_TEST_CASE(Stage1_ReplayCompilePending, Tag::Gpu)
    {
        printf("\n=== Stage 1: a compile-pending root, then recovery ===\n");
        constexpr uint32_t cWidth = 320, cHeight = 180;
        constexpr const wchar_t* cRoot = L"Gamut Map";
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        auto scenario = Dda::MakeScenario(Dda::ScenarioKind::WindowDrag, 64);
        scenario->Start(desktop);
        desktop.UploadAll();

        // The stage's context, with async and runtime compile on.
        ReplayArm incremental;
        EFFECTGRAPH_CONTEXT_DESC desc{ static_cast<EFFECTGRAPH_CONTEXT_FLAGS>(EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE |
                                                                              EFFECTGRAPH_CONTEXT_FLAGS_ALLOW_RUNTIME_COMPILE),
                                       nullptr };
        incremental.ok = SUCCEEDED(EffectGraphCreateContext(g_d3dDevice.get(), &desc, __uuidof(IEffectGraphContextInternal),
                                                            incremental.context.put_void())) &&
                         SUCCEEDED(incremental.context->CreateTextureInput(desktop.Texture(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                           incremental.input.put())) &&
                         SUCCEEDED(incremental.context->CreateEffect(cRoot, incremental.root.put())) &&
                         SUCCEEDED(incremental.root->SetInput(0, incremental.input.get()));
        auto control = MakeArm(desktop.Texture(), cRoot, ArmKind::ForcedWhole);
        TEST("Pending_Built", incremental.ok && control.ok);
        if (!incremental.ok || !control.ok) return;
        incremental.target = MakeSrgbTarget(cWidth, cHeight);

        Dda::StageSim stage(incremental.context.get(), incremental.input.get(), incremental.root.get(),
                            incremental.target.get(), g_d3dContext.get());
        auto surface = MakeClientSurface(cWidth, cHeight);
        stage.AddClient(surface.get());
        // Ticks until a realize succeeds, as the stage's retry tick would.
        const auto tickUntilDrawn = [&](uint32_t& ticks)
        {
            const auto start = std::chrono::steady_clock::now();
            ticks = 0;
            while (FAILED(stage.Tick()) && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
            {
                ++ticks;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return SUCCEEDED(stage.LastRealize());
        };
        uint32_t primeTicks = 0;
        const bool primed = (SUCCEEDED(stage.Prime()) || tickUntilDrawn(primeTicks)) && SUCCEEDED(RealizeWhole(control));
        stage.Acquire(0);
        const auto present = [&](std::vector<RECT>* changed)
        {
            Dda::DesktopFrame frame;
            scenario->Next(desktop, frame);
            desktop.Upload(Dda::ChangedRects(frame));
            if (changed)
                for (const auto& r : Dda::ChangedRects(frame)) changed->push_back(r);
            const HRESULT hr = stage.Present(frame);
            RealizeWhole(control);
            return hr;
        };
        const bool good = primed && SUCCEEDED(present(nullptr)) && SUCCEEDED(present(nullptr));
        stage.Acquire(0);

        // A new baseline the context compiles in the background. White-box:
        // the source gains only a comment, so the node renders the same once
        // compiled, and nothing marks it changed; only the rects reach it.
        auto* rootNode = incremental.context->Model().FindNode(incremental.context->NodeId(incremental.root.get()));
        if (!rootNode || !rootNode->customEffect.has_value())
        {
            TEST("Pending_RootIsCustom", false);
            return;
        }
        rootNode->customEffect->hlslSource += L"\n// compile-pending replay " +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
        rootNode->customEffect->compiledBytecode.clear();
        std::vector<RECT> missed;
        const HRESULT pending = present(&missed);
        const auto timedOut = stage.Acquire(0);
        const HRESULT pendingAgain = present(&missed);
        TEST("Pending_CompilePendingReturned", good && pending == EFFECTGRAPH_E_COMPILE_PENDING);
        TEST("Pending_6_AcquireTimesOut", timedOut.hr == DXGI_ERROR_WAIT_TIMEOUT);

        uint32_t ticks = 0;
        const bool recovered = FAILED(pendingAgain) ? tickUntilDrawn(ticks) : true;
        const auto drawn = stage.LastDrawn();
        size_t uncovered = 0;
        for (const auto& r : missed)
            for (LONG y = (std::max)(r.top, 0L); y < (std::min)(r.bottom, static_cast<LONG>(cHeight)); ++y)
                for (LONG x = (std::max)(r.left, 0L); x < (std::min)(r.right, static_cast<LONG>(cWidth)); ++x)
                    if (!Dda::Inside(drawn, x, y)) ++uncovered;
        const auto reference = Dda::ReadBytes(control.target.get());
        const auto targetDiff = Dda::Compare(Dda::ReadBytes(incremental.target.get()), reference, cWidth, {});
        printf("  [info] compile pending: %s then %s; recovered after %u tick(s), drawing %zu rect(s); "
               "%zu pixels of the waiting presents' rects outside them; %zu differ from the forced-whole render\n",
               pending == EFFECTGRAPH_E_COMPILE_PENDING ? "COMPILE_PENDING" : "another result",
               pendingAgain == EFFECTGRAPH_E_COMPILE_PENDING ? "COMPILE_PENDING" : "drawn", ticks, drawn.size(),
               uncovered, targetDiff.differing);
        TEST("Pending_5_RecoveryRedrawsEarlierRects", recovered && uncovered == 0);
        TEST("Pending_5_RecoveredTargetEqualsForcedWhole", recovered && targetDiff.differing == 0);
        const auto delivered = stage.Acquire(0);
        TEST("Pending_6_ThenDeliversForcedWhole",
             delivered.hr == S_OK && Dda::Compare(Dda::ReadBytes(surface.get()), reference, cWidth, {}).differing == 0);
    }

    // Assertion 7: a client on a second device that queues a copy and
    // releases at once. Each frame it received is read back only after the
    // stage has written the next one into the shared surface.
    SL_TEST_CASE(Stage1_ReplaySecondDeviceClient, Tag::Gpu)
    {
        printf("\n=== Stage 1: a client on a second device ===\n");
        constexpr uint32_t cWidth = 256, cHeight = 144, cAcquires = 300;
        RemoteClient remote;
        std::string error;
        if (!remote.Create(cWidth, cHeight, error))
        {
            printf("  [info] keyed-mutex client unavailable: %s\n", error.c_str());
            TEST("Remote_Created", false);
            return;
        }
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        auto scenario = Dda::MakeScenario(Dda::ScenarioKind::VideoAndScroll, cAcquires);
        scenario->Start(desktop);
        desktop.UploadAll();
        auto incremental = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::Incremental);
        auto control = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::ForcedWhole);
        TEST("Remote_Built", incremental.ok && control.ok);
        if (!incremental.ok || !control.ok) return;
        Dda::StageSim stage(incremental.context.get(), incremental.input.get(), incremental.root.get(),
                            incremental.target.get(), g_d3dContext.get());
        stage.AddClient(remote.StageSurface(), remote.StageMutex());
        if (FAILED(stage.Prime()) || FAILED(RealizeWhole(control))) { TEST("Remote_Primed", false); return; }
        std::vector<uint8_t> pending = Dda::ReadBytes(control.target.get());
        stage.Acquire(0);
        remote.Receive();

        uint32_t delivered = 0, mismatched = 0, failed = 0;
        Dda::DesktopFrame frame;
        while (scenario->Next(desktop, frame))
        {
            desktop.Upload(Dda::ChangedRects(frame));
            if (FAILED(stage.Present(frame)) || FAILED(RealizeWhole(control))) { ++failed; continue; }
            if (FAILED(stage.Acquire(0).hr)) { ++failed; continue; }
            // The previous frame, read now that the stage has overwritten
            // the shared surface.
            if (Dda::Compare(remote.Bytes(), pending, cWidth, {}).differing != 0) ++mismatched;
            ++delivered;
            remote.Receive();
            pending = Dda::ReadBytes(control.target.get());
        }
        if (Dda::Compare(remote.Bytes(), pending, cWidth, {}).differing != 0) ++mismatched;
        printf("  [info] %u acquires on the second device, %u differ from the forced-whole render, %u failed\n",
               delivered, mismatched, failed);
        TEST("Remote_7_EveryAcquireEqualsForcedWhole", delivered >= cAcquires && mismatched == 0 && failed == 0);
    }

    // Assertion 8 (a finding): an _SRGB D2D bitmap over a TYPELESS texture.
    SL_TEST_CASE(Stage1_TypelessSrgbTarget, Tag::Gpu)
    {
        printf("\n=== Stage 1: _SRGB bitmap over a TYPELESS texture ===\n");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 16; desc.Height = 16; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> texture;
        const HRESULT created = g_d3dDevice->CreateTexture2D(&desc, nullptr, texture.put());
        TEST("Typeless_TextureCreated", SUCCEEDED(created));
        if (FAILED(created)) return;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        props.dpiX = props.dpiY = 96.0f;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        const HRESULT hr = g_dc->CreateBitmapFromDxgiSurface(texture.as<IDXGISurface>().get(), &props, bitmap.put());
        int code = -1;
        HRESULT drew = E_FAIL;
        if (SUCCEEDED(hr))
        {
            winrt::com_ptr<ID2D1Image> saved;
            g_dc->GetTarget(saved.put());
            g_dc->SetTarget(bitmap.get());
            g_dc->BeginDraw();
            g_dc->Clear(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f));   // linear 0.5: sRGB code 188
            drew = g_dc->EndDraw();
            g_dc->SetTarget(saved.get());
            if (SUCCEEDED(drew))
            {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                winrt::com_ptr<ID3D11Texture2D> view;
                winrt::check_hresult(g_d3dDevice->CreateTexture2D(&copyDesc, nullptr, view.put()));
                g_d3dContext->CopyResource(view.get(), texture.get());
                code = Dda::ReadBytes(view.get())[0];
            }
        }
        printf("  [info] finding (assertion 8) on %s: CreateBitmapFromDxgiSurface 0x%08X, EndDraw 0x%08X, "
               "linear 0.5 wrote code %d (188 = encoded as sRGB)\n",
               g_useWarp ? "WARP" : "this adapter", static_cast<uint32_t>(hr), static_cast<uint32_t>(drew), code);
    }

    // EvaluatorOptions::pointImageFilter is a switch of its own: off renders
    // image pins through 1.0's linear filter, and changing it on a live
    // context re-renders a clean chain with the new filter, both ways.
    SL_TEST_CASE(Stage1_PointImageFilterSwitch, Tag::Gpu)
    {
        printf("\n=== Stage 1: the point image filter switch (%ls) ===\n", ChainEffect());
        constexpr uint32_t cWidth = 256, cHeight = 144;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        auto scenario = Dda::MakeScenario(Dda::ScenarioKind::WindowDrag, 4);
        scenario->Start(desktop);
        desktop.UploadAll();
        auto realize = [](ReplayArm& arm) {
            RECT drawn[1]{};
            UINT32 count = 0;
            return arm.context.as<IEffectGraphContext2>()->RealizeToIncremental(
                arm.root.get(), arm.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, drawn, 1, &count, nullptr, 0, nullptr);
        };
        auto setFilter = [](ReplayArm& arm, bool point) {
            auto options = arm.context->Evaluator().Options();
            options.pointImageFilter = point;
            arm.context->Evaluator().SetOptions(options);
        };
        ReplayArm linear = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::SwitchOff);
        ReplayArm point = MakeArm(desktop.Texture(), ChainEffect(), ArmKind::SwitchOff);
        TEST("FilterSwitch_Built", linear.ok && point.ok);
        if (!linear.ok || !point.ok) return;
        setFilter(point, true);
        // Two realizes each: a new custom effect needs two passes.
        bool ok = SUCCEEDED(realize(linear)) && SUCCEEDED(realize(linear)) &&
                  SUCCEEDED(realize(point)) && SUCCEEDED(realize(point));
        const auto linearBytes = Dda::ReadBytes(linear.target.get());
        const auto pointBytes = Dda::ReadBytes(point.target.get());
        // The linear arm switched to point with nothing else changed, then back.
        setFilter(linear, true);
        ok = ok && SUCCEEDED(realize(linear));
        const auto switchedOn = Dda::ReadBytes(linear.target.get());
        setFilter(linear, false);
        ok = ok && SUCCEEDED(realize(linear));
        const auto switchedOff = Dda::ReadBytes(linear.target.get());
        const auto filterDiff = Dda::Compare(linearBytes, pointBytes, cWidth, {});
        const auto onDiff = Dda::Compare(switchedOn, pointBytes, cWidth, {});
        const auto offDiff = Dda::Compare(switchedOff, linearBytes, cWidth, {});
        printf("  [info] whole draws, linear against point: %zu of %u px differ; switched on: %zu px from a point "
               "context; switched back: %zu px from the first linear draw\n",
               static_cast<size_t>(filterDiff.differing), cWidth * cHeight, static_cast<size_t>(onDiff.differing),
               static_cast<size_t>(offDiff.differing));
        TEST("FilterSwitch_Realized", ok);
        // Whole draws round the same either way (above), so the switch shows
        // where the filter was found: clipped rects against whole draws. With
        // it off, a 256x144 replay differs from the forced-whole render as 1.0
        // did on WARP; on, it does not (Stage1_ReplaySynthetic). The Adreno
        // X2-90 rounds both filters alike there, so the control is WARP's.
        for (const auto kind : { Dda::ScenarioKind::WindowDrag, Dda::ScenarioKind::VideoAndScroll })
        {
            const std::string name = std::string("FilterSwitch_") + Dda::ScenarioName(kind);
            auto linearScenario = Dda::MakeScenario(kind, 18);
            const auto off = Replay(cWidth, cHeight, SyntheticSource(*linearScenario), false, ReplayFilter::Linear);
            // The same replay, switched to point on the live contexts after
            // their effects were built with the linear filter.
            auto switchedScenario = Dda::MakeScenario(kind, 18);
            const auto switched = Replay(cWidth, cHeight, SyntheticSource(*switchedScenario), false,
                                         ReplayFilter::LinearThenPoint);
            printf("  [info] %s 256x144: with the linear filter %u of %u presents (%zu px) differ from the "
                   "forced-whole render; switched to point after priming, %u (%zu px)\n",
                   Dda::ScenarioName(kind), off.wholeFrames, off.presents, off.wholePixels, switched.wholeFrames,
                   switched.wholePixels);
            TEST((name + "_Replayed").c_str(), off.built && off.presents > 0 && switched.built && switched.presents > 0);
            if (g_useWarp)
                TEST((name + "_OffIsLinear").c_str(), off.wholeFrames > 0);
            else if (off.wholeFrames == 0)
                printf("  [info] %s: the filters render alike on this adapter; the positive control is WARP's\n",
                       Dda::ScenarioName(kind));
            TEST((name + "_LiveSwitchTakes").c_str(), switched.wholeFrames == 0 && switched.clipFrames == 0);
        }
        TEST("FilterSwitch_OnMatchesPointContext", ok && onDiff.differing == 0);
        TEST("FilterSwitch_OffRestoresLinear", ok && offDiff.differing == 0);
    }

    // The recording format round trip, and the self-check the recorder runs:
    // a synthetic scenario written in the recorder's format replays to the
    // same desktop, keyframes rebuild exactly, and a recording whose rects
    // miss a change is caught at the next keyframe.
    SL_TEST_CASE(Stage1_RecordingFormat, Tag::Gpu)
    {
        printf("\n=== Stage 1: the recording format ===\n");
        constexpr uint32_t cWidth = 160, cHeight = 90, cPresents = 25, cInterval = 6;
        ScopedTempDir temp(L"EffectGraphDdaRecording");
        Dda::Desktop written(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        Dda::Desktop broken(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        {
            auto scenario = Dda::MakeScenario(Dda::ScenarioKind::VideoAndScroll, cPresents);
            auto twin = Dda::MakeScenario(Dda::ScenarioKind::WindowDrag, cPresents);
            Dda::RecordingWriter writer(temp.Path() / L"good", cWidth, cHeight, cInterval);
            Dda::RecordingWriter brokenWriter(temp.Path() / L"missed", cWidth, cHeight, cInterval);
            // Frame 0: the desktop as the first acquire returns it, a keyframe.
            scenario->Start(written);
            twin->Start(broken);
            Dda::DesktopFrame first;
            first.dirty = { { 0, 0, static_cast<LONG>(cWidth), static_cast<LONG>(cHeight) } };
            bool ok = writer.Write(first, written) && brokenWriter.Write(first, broken);
            Dda::DesktopFrame frame;
            uint32_t index = 0;
            while (scenario->Next(written, frame))
            {
                Dda::DesktopFrame twinFrame;
                twin->Next(broken, twinFrame);
                ok = writer.Write(frame, written) && ok;
                // The broken twin, a window drag, leaves out the background
                // one frame uncovered, just before a keyframe.
                if (++index == cInterval - 1) twinFrame.dirty.clear();
                ok = brokenWriter.Write(twinFrame, broken) && ok;
            }
            ok = writer.Close() && brokenWriter.Close() && ok;
            TEST("Recording_Written", ok);
        }

        Dda::RecordingReader reader(temp.Path() / L"good");
        TEST("Recording_Opened", reader.Ok());
        if (!reader.Ok()) { printf("  [info] %s\n", reader.Error().c_str()); return; }
        Dda::Desktop replayed(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        Dda::DesktopFrame frame;
        size_t mismatches = 0, total = 0;
        uint32_t frames = 0;
        while (reader.Next(replayed, frame, mismatches)) { total += mismatches; ++frames; }
        TEST("Recording_EveryFrameRead", reader.Ok() && frames == reader.Header().frameCount && frames == cPresents + 1);
        TEST("Recording_KeyframesRebuildExactly", total == 0);
        TEST("Recording_ReplaysToTheSameDesktop", replayed.Texels() == written.Texels());

        // A present with no metadata is recorded whole, as a keyframe, and
        // replays as Whole: the idle-cursor scenario reports one every fifth
        // present.
        {
            Dda::Desktop idle(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
            auto scenario = Dda::MakeScenario(Dda::ScenarioKind::IdleCursor, cPresents);
            Dda::RecordingWriter writer(temp.Path() / L"whole", cWidth, cHeight, cInterval);
            scenario->Start(idle);
            Dda::DesktopFrame first;
            first.dirty = { { 0, 0, static_cast<LONG>(cWidth), static_cast<LONG>(cHeight) } };
            bool ok = writer.Write(first, idle);
            Dda::DesktopFrame next;
            while (scenario->Next(idle, next)) ok = writer.Write(next, idle) && ok;
            ok = writer.Close() && ok;
            Dda::RecordingReader wholeReader(temp.Path() / L"whole");
            Dda::Desktop wholeReplay(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
            size_t wholeMissed = 0;
            uint32_t wholeFrames = 0;
            while (wholeReader.Next(wholeReplay, frame, mismatches))
            {
                wholeMissed += mismatches;
                if (frame.whole) ++wholeFrames;
            }
            printf("  [info] a recording with %u presents carrying no metadata: %zu keyframe pixels flagged\n",
                   wholeFrames, wholeMissed);
            TEST("Recording_NoMetadataPresentIsWhole", ok && wholeReader.Ok() && wholeFrames > 0 && wholeMissed == 0 &&
                                                       wholeReplay.Texels() == idle.Texels());
        }

        Dda::RecordingReader brokenReader(temp.Path() / L"missed");
        Dda::Desktop brokenReplay(g_d3dDevice.get(), g_d3dContext.get(), cWidth, cHeight);
        size_t caught = 0;
        while (brokenReader.Next(brokenReplay, frame, mismatches)) caught += mismatches;
        printf("  [info] a recording missing one frame's exposed background: %zu pixels flagged at the next keyframe\n", caught);
        TEST("Recording_SelfCheckCatchesAMissedRect", caught > 0);
    }

    // Recordings from the PRIV recorder (--recording <dir>): every folder
    // under it holding frames.bin, replayed with the same checks.
    FrameSource RecordingSource(Dda::RecordingReader& reader, ReplayResult*& sink)
    {
        FrameSource source;
        source.start = [&reader, &sink](Dda::Desktop& desktop)
        {
            Dda::DesktopFrame frame;
            size_t mismatches = 0;
            // Frame 0 is the desktop the stage first realizes whole.
            while (reader.Next(desktop, frame, mismatches))
                if (frame.kind == Dda::FrameKind::Present) return true;
            return false;
        };
        source.next = [&reader, &sink](Dda::Desktop& desktop, Dda::DesktopFrame& frame)
        {
            size_t mismatches = 0;
            if (!reader.Next(desktop, frame, mismatches)) return false;
            if (sink) sink->keyframeMismatches += mismatches;
            return true;
        };
        return source;
    }

    // The void rule, from a pass of its own over the recording: more than 5%
    // of presents accumulated, or a keyframe the rects did not rebuild. Each
    // recording case skips a void recording before it replays anything.
    bool RecordingIsVoid(const std::filesystem::path& folder, const std::string& name)
    {
        Dda::RecordingReader reader(folder);
        if (!reader.Ok()) return false;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), reader.Header().width, reader.Header().height);
        Dda::DesktopFrame frame;
        size_t mismatches = 0, missed = 0;
        uint32_t presents = 0, accumulated = 0;
        bool first = true;
        while (reader.Next(desktop, frame, mismatches))
        {
            missed += mismatches;
            if (frame.kind != Dda::FrameKind::Present) continue;
            // Frame 0 is the desktop the stage starts from, not a present it replays.
            if (std::exchange(first, false)) continue;
            ++presents;
            if (frame.accumulatedFrames > 1) ++accumulated;
        }
        const double share = presents ? static_cast<double>(accumulated) / presents : 0.0;
        if (share <= 0.05 && missed == 0) return false;
        printf("  [info] %s: %.1f%% of presents accumulated, %zu keyframe pixels missed -- VOID\n", name.c_str(),
               100.0 * share, missed);
        ReportSkip(name + " is void: " + (missed > 0 ? "a keyframe differs from its rebuild"
                                                     : "more than 5% of presents accumulated"));
        return true;
    }

    SL_TEST_CASE(Stage1_ReplayRecordings, Tag::Gpu)
    {
        printf("\n=== Stage 1: replay of DDA recordings ===\n");
        if (g_recordingPath.empty())
        {
            ReportNotRun("Stage1_ReplayRecordings: needs --recording <dir>");
            return;
        }
        std::error_code ec;
        uint32_t replayed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(g_recordingPath, ec))
        {
            if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / L"frames.bin")) continue;
            Dda::RecordingReader reader(entry.path());
            const std::string name = "Recording_" + entry.path().filename().string();
            if (!reader.Ok())
            {
                printf("  [info] %s: %s\n", name.c_str(), reader.Error().c_str());
                TEST((name + "_Readable").c_str(), false);
                continue;
            }
            ReplayResult* sink = nullptr;
            auto source = RecordingSource(reader, sink);
            ReplayResult keyframes;
            sink = &keyframes;
            auto result = Replay(reader.Header().width, reader.Header().height, source, !g_pluginPath.empty());
            result.keyframeMismatches = keyframes.keyframeMismatches;
            // The void rule: more than 5% of presents accumulated, or a
            // keyframe the rects did not rebuild.
            const double accumulatedShare = result.presents ? static_cast<double>(result.accumulated) / result.presents : 0.0;
            const bool voided = accumulatedShare > 0.05 || result.keyframeMismatches > 0;
            printf("  [info] %s: %ux%u rotation %u, %.1f%% of presents accumulated, %zu keyframe pixels missed%s\n",
                   name.c_str(), reader.Header().width, reader.Header().height, reader.Header().rotation,
                   100.0 * accumulatedShare, result.keyframeMismatches, voided ? " -- VOID" : "");
            if (voided)
            {
                ReportSkip(name + " is void: " +
                           (result.keyframeMismatches > 0 ? "a keyframe differs from its rebuild"
                                                          : "more than 5% of presents accumulated"));
                continue;
            }
            ReportReplay(name, result, false);
            ++replayed;
        }
        printf("  [info] %u recordings replayed from %ls\n", replayed, g_recordingPath.c_str());
        // --recording names recordings to replay: none replayed is a failure,
        // not a pass with nothing checked.
        TEST("Recording_AtLeastOneReplayed", replayed > 0);
    }

    // ---- The numbers: GPU time of a present (realize plus the client copy)
    // against the area it draws, with dirtyRegions off and on, on the same
    // frames. Each frame starts on an idle GPU, as a present-paced stage does.
    class GpuStopwatch
    {
    public:
        GpuStopwatch()
        {
            D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            winrt::check_hresult(g_d3dDevice->CreateQuery(&desc, m_disjoint.put()));
            desc.Query = D3D11_QUERY_TIMESTAMP;
            winrt::check_hresult(g_d3dDevice->CreateQuery(&desc, m_begin.put()));
            winrt::check_hresult(g_d3dDevice->CreateQuery(&desc, m_end.put()));
        }
        void Begin()
        {
            g_d3dContext->Begin(m_disjoint.get());
            g_d3dContext->End(m_begin.get());
        }
        // Milliseconds, or NaN when the counter was disjoint.
        double End()
        {
            g_d3dContext->End(m_end.get());
            g_d3dContext->End(m_disjoint.get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            while (g_d3dContext->GetData(m_disjoint.get(), &disjoint, sizeof(disjoint), 0) == S_FALSE)
                std::this_thread::yield();
            UINT64 begin = 0, end = 0;
            while (g_d3dContext->GetData(m_begin.get(), &begin, sizeof(begin), 0) == S_FALSE) std::this_thread::yield();
            while (g_d3dContext->GetData(m_end.get(), &end, sizeof(end), 0) == S_FALSE) std::this_thread::yield();
            if (disjoint.Disjoint || disjoint.Frequency == 0) return std::nan("");
            return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
        }

    private:
        winrt::com_ptr<ID3D11Query> m_disjoint, m_begin, m_end;
    };

    double Median(std::vector<double> values)
    {
        std::erase_if(values, [](double v) { return std::isnan(v); });
        if (values.empty()) return std::nan("");
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    double Mean(std::vector<double> values)
    {
        std::erase_if(values, [](double v) { return std::isnan(v); });
        if (values.empty()) return std::nan("");
        double sum = 0;
        for (double v : values) sum += v;
        return sum / static_cast<double>(values.size());
    }

    SL_TEST_CASE(Stage1_DrawCostByDirtyArea, Tag::Gpu)
    {
        printf("\n=== Stage 1: present GPU time against dirty area (%ls, %s) ===\n", ChainEffect(),
               g_useWarp ? "WARP: not a hardware number" : "hardware");
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        constexpr uint32_t cWarmup = 4;
        const uint32_t timed = g_useWarp ? 10 : 40;
        GpuStopwatch stopwatch;
        for (const auto kind : cScenarios)
        {
            Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
            auto scenario = Dda::MakeScenario(kind, cWarmup + timed);
            scenario->Start(desktop);
            desktop.UploadAll();
            ReplayArm arms[2] = { MakeArm(desktop.Texture(), ChainEffect(), ArmKind::SwitchOff),
                                  MakeArm(desktop.Texture(), ChainEffect(), ArmKind::Incremental) };
            if (!arms[0].ok || !arms[1].ok) { TEST("Cost_Built", false); return; }
            std::unique_ptr<Dda::StageSim> stages[2];
            winrt::com_ptr<ID3D11Texture2D> surfaces[2];
            for (int a = 0; a < 2; ++a)
            {
                stages[a] = std::make_unique<Dda::StageSim>(arms[a].context.get(), arms[a].input.get(), arms[a].root.get(),
                                                            arms[a].target.get(), g_d3dContext.get());
                surfaces[a] = MakeClientSurface(width, height);
                stages[a]->AddClient(surfaces[a].get());
                stages[a]->Prime();
                stages[a]->Acquire(0);
            }
            std::vector<double> ms[2], cpuMs[2], wholeMs[2], rectMs[2];
            uint32_t frameIndex = 0, failures = 0, collapsed = 0, notLive[2]{}, drawnMisses = 0;
            const Rendering::RegionDrawStats statsBefore[2] = { arms[0].context->Evaluator().RegionStats(),
                                                                arms[1].context->Evaluator().RegionStats() };
            double changedArea = 0, boxedArea = 0;
            Dda::DesktopFrame frame;
            while (scenario->Next(desktop, frame))
            {
                if (frame.kind != Dda::FrameKind::Present) continue;
                if (frame.whole) desktop.UploadAll();
                else desktop.Upload(Dda::ChangedRects(frame));
                const auto changedRects = Dda::ChangedRects(frame);
                // Alternate which arm goes first.
                for (int step = 0; step < 2; ++step)
                {
                    const int a = (frameIndex + step) % 2;
                    auto& evaluator = arms[a].context->Evaluator();
                    const uint32_t inputId = arms[a].context->NodeId(arms[a].input.get());
                    const uint32_t rootId = arms[a].context->NodeId(arms[a].root.get());
                    const uint64_t inputChanges = evaluator.OutputChangeCount(inputId);
                    const uint64_t rootChanges = evaluator.OutputChangeCount(rootId);
                    WaitForGpuIdle();
                    stopwatch.Begin();
                    const auto cpuStart = std::chrono::steady_clock::now();
                    const HRESULT hr = stages[a]->Present(frame);
                    const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
                    stages[a]->Acquire(0);
                    const double elapsed = stopwatch.End();
                    if (FAILED(hr)) ++failures;
                    // Section 6 (a) and (d): the source and the chain end
                    // changed on this present, in both arms, so neither is
                    // served from D2D's cache.
                    if (evaluator.OutputChangeCount(inputId) == inputChanges ||
                        evaluator.OutputChangeCount(rootId) == rootChanges)
                        ++notLive[a];
                    if (a == 1 && !DrawnAsExpected(changedRects, frame.whole, stages[1]->LastDrawn(), width, height))
                        ++drawnMisses;
                    if (frameIndex >= cWarmup)
                    {
                        ms[a].push_back(elapsed);
                        cpuMs[a].push_back(cpu);
                        (frame.whole ? wholeMs[a] : rectMs[a]).push_back(elapsed);
                    }
                }
                // Section 6 (c): the area the bounding box adds when > 16 rects.
                const auto changed = Dda::ChangedRects(frame);
                if (changed.size() > 16)
                {
                    ++collapsed;
                    Graph::DirtyRegion region;
                    for (const auto& r : changed) region.Add({ r.left, r.top, r.right, r.bottom }, 1);
                    double unionArea = 0;
                    {
                        // The rects' own union, without the cap.
                        std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0);
                        for (const auto& r : changed)
                            for (LONG y = (std::max)(r.top, 0L); y < (std::min)(r.bottom, static_cast<LONG>(height)); ++y)
                                std::memset(&mask[static_cast<size_t>(y) * width + (std::max)(r.left, 0L)], 1,
                                            static_cast<size_t>((std::min)(r.right, static_cast<LONG>(width)) - (std::max)(r.left, 0L)));
                        for (uint8_t m : mask) unionArea += m;
                    }
                    changedArea += unionArea;
                    boxedArea += static_cast<double>(region.UnionArea());
                }
                ++frameIndex;
            }
            // Section 6 (b): RegionPixelsDrawn/Target per arm, as pixel counts.
            uint64_t drawnPixels[2]{}, targetPixels[2]{};
            for (int a = 0; a < 2; ++a)
            {
                const auto& stats = arms[a].context->Evaluator().RegionStats();
                drawnPixels[a] = stats.pixelsDrawn - statsBefore[a].pixelsDrawn;
                targetPixels[a] = stats.pixelsTarget - statsBefore[a].pixelsTarget;
            }
            const double drawnShare = targetPixels[1] ? static_cast<double>(drawnPixels[1]) / static_cast<double>(targetPixels[1]) : 0.0;
            const double off = Median(ms[0]), on = Median(ms[1]);
            const double offCpu = Median(cpuMs[0]), onCpu = Median(cpuMs[1]);
            printf("  [info] %-15s %ux%u: drew %llu of %llu target px (%.6f), switch off %llu of %llu; median per present: "
                   "GPU off %.3f, on %.3f ms; realize CPU off %.3f, on %.3f ms (%zu timed, %u failed)\n",
                   Dda::ScenarioName(kind), width, height, static_cast<unsigned long long>(drawnPixels[1]),
                   static_cast<unsigned long long>(targetPixels[1]), drawnShare,
                   static_cast<unsigned long long>(drawnPixels[0]), static_cast<unsigned long long>(targetPixels[0]),
                   off, on, offCpu, onCpu, ms[1].size(), failures);
            printf("  [info] %-15s instrument: %u/%u presents with no output change (off/on), %u drew other than "
                   "what changed\n", Dda::ScenarioName(kind), notLive[0], notLive[1], drawnMisses);
            // A scenario that mixes presents with no metadata (drawn whole)
            // and presents with rects has a two-group median: report each.
            if (!wholeMs[1].empty() && !rectMs[1].empty())
                printf("  [info] %-15s by present: %zu with rects, median GPU off %.3f, on %.3f ms; %zu with no "
                       "metadata, off %.3f, on %.3f ms; mean GPU off %.3f, on %.3f ms\n",
                       Dda::ScenarioName(kind), rectMs[1].size(), Median(rectMs[0]), Median(rectMs[1]),
                       wholeMs[1].size(), Median(wholeMs[0]), Median(wholeMs[1]), Mean(ms[0]), Mean(ms[1]));
            if (collapsed)
                printf("  [info] %-15s %u frames over 16 rects: the bounding box adds %.1f%% to the rects' union\n",
                       Dda::ScenarioName(kind), collapsed, changedArea > 0 ? 100.0 * (boxedArea - changedArea) / changedArea : 0.0);
            const std::string prefix = std::string("Cost_") + Dda::ScenarioName(kind);
            TEST((prefix + "_Realized").c_str(), failures == 0 && !ms[1].empty());
            TEST((prefix + "_LiveInBothArms").c_str(), notLive[0] == 0 && notLive[1] == 0);
            TEST((prefix + "_SwitchOffDrawsWhole").c_str(), targetPixels[0] > 0 && drawnPixels[0] == targetPixels[0]);
            TEST((prefix + "_DrawsWhatChanged").c_str(), drawnMisses == 0);
            if (kind == Dda::ScenarioKind::CoverAll)
            {
                // Section 6 (b): a frame whose rects cover everything draws
                // the whole target and costs no more than the switch off, on
                // the GPU and in the CPU the region bookkeeping adds.
                TEST("Cost_CoverAll_DrawsWhole", targetPixels[1] > 0 && drawnPixels[1] == targetPixels[1]);
                // Each present times both arms back to back, so the pairs
                // share a clock state: fail only when on is dearer than
                // 1.05 x off + 0.05 ms with 99% confidence.
                const auto gpu = ComparePaired(ms[1], ms[0], 1.05, 0.05);
                const auto cpu = ComparePaired(cpuMs[1], cpuMs[0], 1.05, 0.05);
                const auto gpuRaw = ComparePaired(ms[1], ms[0]);
                const auto cpuRaw = ComparePaired(cpuMs[1], cpuMs[0]);
                PrintPairedTiming("CoverAll GPU on - off", gpuRaw);
                PrintPairedTiming("CoverAll CPU on - off", cpuRaw);
                TIMING_TEST("Cost_CoverAll_NoDearerThanOff", !gpu.ConfidentlyAbove());
                TIMING_TEST("Cost_CoverAll_CpuNoDearerThanOff", !cpu.ConfidentlyAbove());
            }
        }
    }
}

// ---- Stage 2: the exact gate on the DDA graph -------------------------------
//
// The DDA graph (texture input -> Luminance Statistics -> tone map, Working
// Space giving ClipNits and SdrWhiteNits, SourcePeakNits <- Max,
// WhiteErrorBudget <- WhiteErrorBudget) replayed over the synthetic
// scenarios in three arms on the same frames: the gate with the readback
// skip off (every dispatch's values on the CPU, so the gate judges each
// one: the change-rate table), a forced-whole reference (dirtyRegions off,
// nothing cached) every present is checked against, and the gate with the
// skip on, the stage's configuration (the timing). With --plugin the tone
// map is the HDR Screenshot Tonemap; without it a per-pixel stand-in bound
// the same way.
namespace
{
    // The stand-in: an exposure from SdrWhiteNits / SourcePeakNits, dimmed by
    // the fraction of WhiteErrorBudget * 1024, so a budget move of 1e-3 shows
    // in 8-bit pixels; both statistics bindings gpu-bindable.
    Effects::ShaderLabEffectDescriptor GateToneMapDescriptor()
    {
        Effects::ShaderLabEffectDescriptor d;
        d.name = L"Stage2 Gate Tone Map";
        d.effectId = L"Stage2 Gate Tone Map";
        d.effectVersion = 1;
        d.shaderType = Graph::CustomShaderType::PixelShader;
        d.inputNames = { L"Source" };
        d.shiftInvariant = true;   // reads only its own pixel, and does not dither
        d.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(SourcePeakNits, t1)
SHADERLAB_GPU_BUFFER(WhiteErrorBudget, t2)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, SourcePeakNits)
    SHADERLAB_PARAM(float, WhiteErrorBudget)
    float SdrWhiteNits;
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, SourcePeakNits)
    SHADERLAB_LOAD_PARAM(float, WhiteErrorBudget)
    float white = SdrWhiteNits;
    float4 c = Source.Sample(InputSampler, uv0.xy);
    float exposure = white / max(SourcePeakNits, white);
    float dim = 1.0 - 0.5 * frac(WhiteErrorBudget * 1024.0);
    return float4(c.rgb * exposure * dim, c.a);
}
)HLSL";
        d.parameters = {
            Graph::ParameterDefinition{ L"SourcePeakNits", L"float", 1000.0f, 1.0f, 10000.0f, 1.0f, {}, L"", true },
            Graph::ParameterDefinition{ L"WhiteErrorBudget", L"float", 0.0f, 0.0f, 100.0f, 0.1f, {}, L"", true },
            Graph::ParameterDefinition{ L"SdrWhiteNits", L"float", 200.0f, 1.0f, 10000.0f, 1.0f },
        };
        return d;
    }

    const wchar_t* GateToneMapName() { return g_pluginPath.empty() ? L"Stage2 Gate Tone Map" : cTonemapName; }

    enum class GateArmKind
    {
        SkipOff,        // gate on, readback skip off: the change rates
        Reference,      // dirtyRegions off, nothing cached, skip off
        SkipOn,         // gate on, readback skip on: the stage, timed
    };

    struct GateArm
    {
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::com_ptr<IEffectGraphNode> input, workingSpace, stats, root;
        winrt::com_ptr<ID3D11Texture2D> target;
        std::unique_ptr<Dda::StageSim> stage;
        uint32_t statsId{ 0 }, inputId{ 0 }, rootId{ 0 };
        bool ok{ false };
    };

    GateArm MakeGateArm(ID3D11Texture2D* desktop, GateArmKind kind)
    {
        GateArm arm;
        arm.context = CreateStageContext();
        auto options = arm.context->Evaluator().Options();
        options.dirtyRegions = kind != GateArmKind::Reference;
        options.effectOutputCaching = kind != GateArmKind::Reference;
        options.skipUnneededCpuReadback = kind == GateArmKind::SkipOn;
        options.analysisChangeGate = true;
        arm.context->Evaluator().SetOptions(options);

        auto& context = *arm.context;
        bool ok = SUCCEEDED(context.CreateTextureInput(desktop, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, arm.input.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Working Space", arm.workingSpace.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Luminance Statistics", arm.stats.put()));
        if (ok && g_pluginPath.empty())
        {
            static const auto descriptor = GateToneMapDescriptor();
            const uint32_t id = context.Model().AddNode(Effects::ShaderLabEffects::CreateNode(descriptor));
            ok = context.NodeFor(id, arm.root.put()) == S_OK;
        }
        else if (ok)
        {
            ok = SUCCEEDED(context.CreateEffect(cTonemapName, arm.root.put()));
        }
        ok = ok && SUCCEEDED(arm.stats->SetInput(0, arm.input.get())) &&
             SUCCEEDED(arm.root->SetInput(0, arm.input.get())) &&
             SUCCEEDED(arm.stats->Bind(L"ClipNits", arm.workingSpace.get(), L"SdrWhiteNits")) &&
             SUCCEEDED(arm.root->Bind(L"SdrWhiteNits", arm.workingSpace.get(), L"SdrWhiteNits")) &&
             SUCCEEDED(arm.root->Bind(L"SourcePeakNits", arm.stats.get(), L"Max")) &&
             SUCCEEDED(arm.root->Bind(L"WhiteErrorBudget", arm.stats.get(), L"WhiteErrorBudget"));
        if (!ok) return arm;
        D3D11_TEXTURE2D_DESC desc{};
        desktop->GetDesc(&desc);
        arm.target = MakeSrgbTarget(desc.Width, desc.Height);
        arm.stage = std::make_unique<Dda::StageSim>(arm.context.get(), arm.input.get(), arm.root.get(), arm.target.get(),
                                                    g_d3dContext.get());
        arm.statsId = context.NodeId(arm.stats.get());
        arm.inputId = context.NodeId(arm.input.get());
        arm.rootId = context.NodeId(arm.root.get());
        arm.ok = true;
        return arm;
    }

    struct GateReplayResult
    {
        bool built{ false };
        uint32_t presents{ 0 }, failed{ 0 }, rectPresents{ 0 };
        // Presents in any arm whose FallbackReadbacks moved; fallback builds.
        uint32_t fallbackFrames{ 0 };
        uint64_t fallbackBuilds{ 0 };
        // Presents whose target differs from the reference: skip-off arm, skip-on arm.
        uint32_t mismatches[2]{};
        // The skip-off arm's verdicts, and presents with an unchanged verdict
        // that drew other than what changed.
        Rendering::BindingGateStats gate;
        uint32_t unchangedMisdrawn{ 0 };
        // Presents (of those judged) on which Max and WhiteErrorBudget kept
        // their bits, in the skip-off arm.
        uint32_t maxSame{ 0 }, budgetSame{ 0 };
        // Rects-verdict presents on which WhiteErrorBudget moved while Max
        // kept its bits (the control for a field other than Max), and presents
        // whose verdict or draw disagrees with the field movement: a move needs
        // 'changed' and a whole draw, no move needs 'unchanged'.
        uint32_t budgetOnlyMoves{ 0 }, misjudged{ 0 };
        // The range each field took, so a field that never moved is visible as such.
        float maxLow{ INFINITY }, maxHigh{ -INFINITY }, budgetLow{ INFINITY }, budgetHigh{ -INFINITY };
        // The skip-on arm: verdicts it could judge (none expected: its values
        // do not reach the CPU), and the CPU time of each Rects present.
        uint64_t skipOnJudged{ 0 }, skipOnNoValue{ 0 }, skipOnWhole{ 0 };
        // Instrument for the timed arm: presents on which its source or chain
        // end did not change, or its statistics did not dispatch exactly once
        // (one gate verdict and one skipped readback).
        uint32_t skipOnNotLive{ 0 };
        // Fallback readbacks while priming (Prime and the extra Tick), all arms.
        uint64_t primeFallbackReadbacks{ 0 };
        std::vector<double> rectCpuMs, coveringCpuMs;
        // Instrument: presents whose source or chain end did not change in the
        // skip-off arm.
        uint32_t notLive{ 0 };
    };

    Rendering::BindingGateStats GateDelta(const Rendering::BindingGateStats& after, const Rendering::BindingGateStats& before)
    {
        return { after.whole - before.whole, after.unchanged - before.unchanged, after.changed - before.changed,
                 after.noValue - before.noValue };
    }

    GateReplayResult GateReplay(uint32_t width, uint32_t height, const FrameSource& source)
    {
        GateReplayResult result;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        if (!source.start(desktop)) return result;
        GateArm arms[3] = { MakeGateArm(desktop.Texture(), GateArmKind::SkipOff),
                            MakeGateArm(desktop.Texture(), GateArmKind::Reference),
                            MakeGateArm(desktop.Texture(), GateArmKind::SkipOn) };
        if (!arms[0].ok || !arms[1].ok || !arms[2].ok) return result;
        auto& skipOff = arms[0];
        auto& reference = arms[1];
        auto& skipOn = arms[2];
        uint64_t primeFallbackBefore[3]{};
        for (int a = 0; a < 3; ++a) primeFallbackBefore[a] = arms[a].context->Evaluator().ReadbackStats().fallbackReadbacks;
        for (auto& arm : arms)
            if (FAILED(arm.stage->Prime())) return result;
        // A second frame with nothing new: lane 3 serves from it.
        for (auto& arm : arms)
            if (FAILED(arm.stage->Tick())) return result;
        for (int a = 0; a < 3; ++a)
            result.primeFallbackReadbacks +=
                arms[a].context->Evaluator().ReadbackStats().fallbackReadbacks - primeFallbackBefore[a];
        result.built = true;
        const auto field = [](GateArm& arm, const wchar_t* name)
        {
            return AnalysisField(arm.context->Model().FindNode(arm.statsId), name);
        };
        float lastMax = field(skipOff, L"Max"), lastBudget = field(skipOff, L"WhiteErrorBudget");
        const auto gateStart = skipOff.context->Evaluator().BindingGate();
        const auto skipOnGateStart = skipOn.context->Evaluator().BindingGate();

        Dda::DesktopFrame frame;
        while (source.next(desktop, frame))
        {
            if (frame.kind == Dda::FrameKind::NoFrame) continue;
            ++result.presents;
            const auto changed = Dda::ChangedRects(frame);
            const bool covering = frame.whole || CoversSurface(changed, width, height);
            if (!covering) ++result.rectPresents;

            uint64_t fallbackBefore[3]{};
            for (int a = 0; a < 3; ++a) fallbackBefore[a] = arms[a].context->Evaluator().ReadbackStats().fallbackReadbacks;

            // The skip-off arm, and its verdict on this present.
            auto& evaluator = skipOff.context->Evaluator();
            const auto gateBefore = evaluator.BindingGate();
            const uint64_t inputChanges = evaluator.OutputChangeCount(skipOff.inputId);
            const uint64_t rootChanges = evaluator.OutputChangeCount(skipOff.rootId);
            HRESULT hr = skipOff.stage->Present(frame);
            const auto verdict = GateDelta(evaluator.BindingGate(), gateBefore);
            if (evaluator.OutputChangeCount(skipOff.inputId) == inputChanges ||
                evaluator.OutputChangeCount(skipOff.rootId) == rootChanges)
                ++result.notLive;
            if (verdict.unchanged && !verdict.changed && !verdict.whole && !verdict.noValue &&
                !DrawnAsExpected(changed, frame.whole, skipOff.stage->LastDrawn(), width, height))
                ++result.unchangedMisdrawn;
            const float max = field(skipOff, L"Max"), budget = field(skipOff, L"WhiteErrorBudget");
            const bool maxMoved = std::memcmp(&max, &lastMax, sizeof(float)) != 0;
            const bool budgetMoved = std::memcmp(&budget, &lastBudget, sizeof(float)) != 0;
            if (!maxMoved) ++result.maxSame;
            if (!budgetMoved) ++result.budgetSame;
            if (!verdict.whole && !verdict.noValue)
            {
                const bool moved = maxMoved || budgetMoved;
                if (budgetMoved && !maxMoved) ++result.budgetOnlyMoves;
                if (verdict.changed != (moved ? 1u : 0u) || verdict.unchanged != (moved ? 0u : 1u) ||
                    (moved && !DrawnAsExpected(changed, true, skipOff.stage->LastDrawn(), width, height)))
                    ++result.misjudged;
            }
            lastMax = max;
            lastBudget = budget;
            result.maxLow = (std::min)(result.maxLow, max);
            result.maxHigh = (std::max)(result.maxHigh, max);
            result.budgetLow = (std::min)(result.budgetLow, budget);
            result.budgetHigh = (std::max)(result.budgetHigh, budget);

            if (SUCCEEDED(hr)) hr = reference.stage->Present(frame);

            // The skip-on arm, timed from an idle GPU.
            auto& skipOnEvaluator = skipOn.context->Evaluator();
            const auto skipOnGateBefore = skipOnEvaluator.BindingGate();
            const uint64_t skipOnSkippedBefore = skipOnEvaluator.ReadbackStats().skippedReadbacks;
            const uint64_t skipOnInputChanges = skipOnEvaluator.OutputChangeCount(skipOn.inputId);
            const uint64_t skipOnRootChanges = skipOnEvaluator.OutputChangeCount(skipOn.rootId);
            WaitForGpuIdle();
            const auto start = std::chrono::steady_clock::now();
            if (SUCCEEDED(hr)) hr = skipOn.stage->Present(frame);
            const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            (covering ? result.coveringCpuMs : result.rectCpuMs).push_back(cpuMs);
            const auto skipOnVerdict = GateDelta(skipOnEvaluator.BindingGate(), skipOnGateBefore);
            if (skipOnEvaluator.OutputChangeCount(skipOn.inputId) == skipOnInputChanges ||
                skipOnEvaluator.OutputChangeCount(skipOn.rootId) == skipOnRootChanges ||
                skipOnVerdict.whole + skipOnVerdict.unchanged + skipOnVerdict.changed + skipOnVerdict.noValue != 1 ||
                skipOnEvaluator.ReadbackStats().skippedReadbacks != skipOnSkippedBefore + 1)
                ++result.skipOnNotLive;
            if (FAILED(hr))
            {
                ++result.failed;
                continue;
            }

            for (int a = 0; a < 3; ++a)
                if (arms[a].context->Evaluator().ReadbackStats().fallbackReadbacks != fallbackBefore[a])
                {
                    ++result.fallbackFrames;
                    break;
                }
            const auto expected = Dda::ReadBytes(reference.target.get());
            if (Dda::Compare(Dda::ReadBytes(skipOff.target.get()), expected, width, {}).differing) ++result.mismatches[0];
            if (Dda::Compare(Dda::ReadBytes(skipOn.target.get()), expected, width, {}).differing) ++result.mismatches[1];
        }
        result.gate = GateDelta(skipOff.context->Evaluator().BindingGate(), gateStart);
        const auto skipOnGate = GateDelta(skipOn.context->Evaluator().BindingGate(), skipOnGateStart);
        result.skipOnJudged = skipOnGate.unchanged + skipOnGate.changed;
        result.skipOnNoValue = skipOnGate.noValue;
        result.skipOnWhole = skipOnGate.whole;
        // Whether missing or still compiling, every fallback build counts.
        for (auto& arm : arms)
        {
            const auto& stats = arm.context->Evaluator().ReadbackStats();
            result.fallbackBuilds += stats.fallbackBuilds + stats.fallbackBuildsPending;
        }
        return result;
    }

    void ReportGateReplay(const std::string& name, const GateReplayResult& r)
    {
        const auto share = [&](uint32_t n) { return r.presents ? static_cast<double>(n) / r.presents : 0.0; };
        printf("  [info] %s: %u presents (%u with rects), %u failed; skip-off verdicts: %llu unchanged, %llu changed, "
               "%llu whole, %llu no value\n", name.c_str(), r.presents, r.rectPresents, r.failed,
               static_cast<unsigned long long>(r.gate.unchanged), static_cast<unsigned long long>(r.gate.changed),
               static_cast<unsigned long long>(r.gate.whole), static_cast<unsigned long long>(r.gate.noValue));
        printf("  [info] %s: bit-unchanged on %.3f of presents for Max (%.3f to %.3f nits), %.3f for WhiteErrorBudget "
               "(%.6f to %.6f)\n", name.c_str(),
               share(r.maxSame), r.maxLow, r.maxHigh, share(r.budgetSame), r.budgetLow, r.budgetHigh);
        printf("  [info] %s: %u Rects-verdict presents moved WhiteErrorBudget alone; %u verdicts disagree with the "
               "bound fields' movement\n", name.c_str(), r.budgetOnlyMoves, r.misjudged);
        printf("  [info] %s: skip on, gate on: Rects-present realize CPU median %.3f ms, mean %.3f ms over %zu; covering "
               "presents median %.3f ms over %zu; %llu verdicts judged, %llu without a value (%s)\n", name.c_str(),
               Median(r.rectCpuMs), Mean(r.rectCpuMs), r.rectCpuMs.size(), Median(r.coveringCpuMs), r.coveringCpuMs.size(),
               static_cast<unsigned long long>(r.skipOnJudged), static_cast<unsigned long long>(r.skipOnNoValue),
               g_useWarp ? "WARP: not a hardware number" : "hardware");
        printf("  [info] %s: %u presents with a fallback readback, %llu fallback build(s); targets off the reference: "
               "skip off %u, skip on %u; %u presents not live\n", name.c_str(), r.fallbackFrames,
               static_cast<unsigned long long>(r.fallbackBuilds), r.mismatches[0], r.mismatches[1], r.notLive);
        TEST((name + "_Built").c_str(), r.built && r.presents > 0 && r.failed == 0);
        TEST((name + "_NoFallbackReadbackAnyFrame").c_str(), r.fallbackFrames == 0 && r.fallbackBuilds == 0);
        TEST((name + "_TargetsEqualForcedWhole").c_str(), r.mismatches[0] == 0 && r.mismatches[1] == 0);
        TEST((name + "_UnchangedDrawsWhatChanged").c_str(), r.unchangedMisdrawn == 0);
        TEST((name + "_VerdictFollowsEveryBoundField").c_str(), r.misjudged == 0);
        TEST((name + "_LivePipeline").c_str(), r.notLive == 0);
        // Every present dispatched the statistics once, and the skip-off
        // arm gave each a verdict; the skip-on arm can judge none.
        TEST((name + "_EveryPresentJudged").c_str(),
             r.gate.unchanged + r.gate.changed + r.gate.whole + r.gate.noValue == r.presents && r.gate.noValue == 0);
        printf("  [info] %s: skip on: %llu no value, %llu whole over %u presents (%u with rects); %u presents not live; "
               "%llu fallback readback(s) while priming\n", name.c_str(),
               static_cast<unsigned long long>(r.skipOnNoValue), static_cast<unsigned long long>(r.skipOnWhole),
               r.presents, r.rectPresents, r.skipOnNotLive, static_cast<unsigned long long>(r.primeFallbackReadbacks));
        TEST((name + "_NoFallbackReadbackPriming").c_str(), r.primeFallbackReadbacks == 0);
        // The timed arm ran its statistics on every present: its CPU time is
        // not that of a pass that skipped them.
        TEST((name + "_SkipOnLivePipeline").c_str(), r.skipOnNotLive == 0);
        TEST((name + "_SkipOnHasNoValue").c_str(), r.skipOnJudged == 0 && r.skipOnNoValue == r.rectPresents &&
                                                   r.skipOnNoValue + r.skipOnWhole == r.presents);
    }

    SL_TEST_CASE(Stage2_GateReplaySynthetic, Tag::Gpu)
    {
        printf("\n=== Stage 2: the exact gate, DDA graph, synthetic scenarios (%ls) ===\n", GateToneMapName());
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        uint32_t budgetOnlyMoves = 0;
        for (const auto kind : cScenarios)
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = GateReplay(width, height, SyntheticSource(*scenario));
            ReportGateReplay(std::string("Gate_") + Dda::ScenarioName(kind), result);
            budgetOnlyMoves += result.budgetOnlyMoves;
        }
        // Positive control: a present judged over rects moved a bound field
        // other than Max, so VerdictFollowsEveryBoundField saw one.
        TEST("Gate_SomeVerdictOnABudgetOnlyMove", budgetOnlyMoves > 0);
    }

    SL_TEST_CASE(Stage2_GateReplayRecordings, Tag::Gpu)
    {
        printf("\n=== Stage 2: the exact gate over DDA recordings ===\n");
        if (g_recordingPath.empty())
        {
            ReportNotRun("Stage2_GateReplayRecordings: needs --recording <dir>");
            return;
        }
        std::error_code ec;
        uint32_t replayed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(g_recordingPath, ec))
        {
            if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / L"frames.bin")) continue;
            Dda::RecordingReader reader(entry.path());
            const std::string name = "GateRecording_" + entry.path().filename().string();
            if (!reader.Ok())
            {
                printf("  [info] %s: %s\n", name.c_str(), reader.Error().c_str());
                TEST((name + "_Readable").c_str(), false);
                continue;
            }
            if (RecordingIsVoid(entry.path(), name)) continue;
            ReplayResult* sink = nullptr;
            auto source = RecordingSource(reader, sink);
            ReportGateReplay(name, GateReplay(reader.Header().width, reader.Header().height, source));
            ++replayed;
        }
        TEST("GateRecording_AtLeastOneReplayed", replayed > 0);
    }
}

// ---- Stage 3: deferred commit and commit policy ----------------------------
//
// The DDA graph's statistics under EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT:
// each dispatch is read back without waiting and committed, per field, when
// it lands. The mechanism cases drive a small desktop by hand; the replay
// runs the simulated stage (poll first, the provisional damping, the retry
// tick) and the drain arm over the synthetic scenarios.
namespace
{
    winrt::com_ptr<IEffectGraphContextInternal> CreateDeferredContext(
        EFFECTGRAPH_CONTEXT_FLAGS extra = EFFECTGRAPH_CONTEXT_FLAGS_NONE)
    {
        const EFFECTGRAPH_CONTEXT_DESC desc{
            static_cast<EFFECTGRAPH_CONTEXT_FLAGS>(EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT | extra), nullptr };
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), &desc,
            __uuidof(IEffectGraphContextInternal), context.put_void()));
        auto profile = Rendering::PresetP3_1000();
        profile.caps.sdrWhiteLevelNits = cSdrWhiteNits;
        context->SetDisplayProfile(profile);
        return context;
    }

    // scRGB greys: 80 nits; 1000, 2000 and 3000-nit peaks; one over ClipNits
    // (SdrWhiteNits * 1.01) but under every peak, which moves WhiteErrorBudget only.
    constexpr float cBackground = 1.0f;
    constexpr float cPeak1000 = 12.5f;
    constexpr float cPeak2000 = 25.0f;
    constexpr float cPeak3000 = 37.5f;
    constexpr float cClipped = 3.5f;

    bool Near(float value, float expected, float tolerance = 0.5f)
    {
        return std::isfinite(value) && std::fabs(value - expected) <= tolerance;
    }

    bool SameFloat(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

    struct ReadbackCounts
    {
        uint64_t fallback{ 0 }, unskipped{ 0 }, fresh{ 0 };
        bool operator==(const ReadbackCounts&) const = default;
    };
    ReadbackCounts CountsOf(const Rendering::AnalysisReadbackStats& stats)
    {
        return { stats.fallbackReadbacks, stats.unskippedReadbacks, stats.freshReadbacks };
    }

    // The DDA graph on a small desktop: a 1000-nit pixel at (5, 5) on 80 nits.
    struct DeferredRig
    {
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::com_ptr<IEffectGraphContext2> context2;
        std::unique_ptr<Dda::Desktop> desktop;
        winrt::com_ptr<IEffectGraphNode> input, workingSpace, stats, root;
        winrt::com_ptr<IEffectGraphInput2> input2;
        winrt::com_ptr<IEffectGraphNode2> stats2, root2;
        winrt::com_ptr<ID3D11Texture2D> target;
        uint32_t statsId{ 0 }, rootId{ 0 }, inputId{ 0 };
        UINT32 maxIndex{ 0 }, budgetIndex{ 0 };
        uint32_t width{ 0 }, height{ 0 };
        std::vector<RECT> drawn;
        bool whole{ false };
        bool ok{ false };

        Rendering::GraphEvaluator& Evaluator() { return context->Evaluator(); }
        Graph::GraphModel& Model() { return context->Model(); }
        uint64_t Changes() { return Evaluator().OutputChangeCount(statsId); }

        HRESULT Realize()
        {
            RECT rects[16]{};
            UINT32 count = 0;
            const HRESULT hr = context2->RealizeToIncremental(root.get(), target.get(),
                EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, rects, 16, &count, nullptr, 0, nullptr);
            drawn.assign(rects, rects + (SUCCEEDED(hr) ? count : 0));
            whole = drawn.size() == 1 && drawn[0].left == 0 && drawn[0].top == 0 &&
                    drawn[0].right == static_cast<LONG>(width) && drawn[0].bottom == static_cast<LONG>(height);
            return hr;
        }
        // Realizes from an idle GPU until nothing is left to collect; the
        // whole draws among them.
        HRESULT Drain(uint32_t* wholeDraws = nullptr)
        {
            HRESULT hr = S_FALSE;
            uint32_t wholes = 0;
            for (int i = 0; i < 16 && hr == S_FALSE; ++i)
            {
                WaitForGpuIdle();
                hr = Realize();
                if (whole) ++wholes;
            }
            if (wholeDraws) *wholeDraws = wholes;
            return hr;
        }
        void Fill(LONG x0, LONG y0, LONG x1, LONG y1, float grey)
        {
            for (LONG y = y0; y < y1; ++y)
                for (LONG x = x0; x < x1; ++x)
                    desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), grey, grey, grey);
            desktop->Upload({ RECT{ x0, y0, x1, y1 } });
        }
        // Paints a rect and reports it.
        void Paint(LONG x0, LONG y0, LONG x1, LONG y1, float grey)
        {
            Fill(x0, y0, x1, y1, grey);
            const RECT rect{ x0, y0, x1, y1 };
            input2->InvalidateRects(&rect, 1);
        }
        // Paints a rect and reports the whole input.
        void PaintWhole(LONG x0, LONG y0, LONG x1, LONG y1, float grey)
        {
            Fill(x0, y0, x1, y1, grey);
            input2->Invalidate();
        }
        // The CPU image with a grey rect, as a texture made from it. Shown
        // later by a GPU copy: an upload into a texture queued work still
        // reads can wait for that work (WARP does), which would land a
        // readback the case means to keep in flight.
        winrt::com_ptr<ID3D11Texture2D> Snapshot(LONG x0, LONG y0, LONG x1, LONG y1, float grey)
        {
            for (LONG y = y0; y < y1; ++y)
                for (LONG x = x0; x < x1; ++x)
                    desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), grey, grey, grey);
            D3D11_TEXTURE2D_DESC desc{};
            desktop->Texture()->GetDesc(&desc);
            const D3D11_SUBRESOURCE_DATA data{ desktop->Texels().data(), desc.Width * 8, 0 };
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, &data, texture.put()));
            return texture;
        }
        // Copies a snapshot into the desktop on the GPU and reports `rect`, or
        // the whole input.
        void Show(ID3D11Texture2D* snapshot, const RECT* rect)
        {
            g_d3dContext->CopyResource(desktop->Texture(), snapshot);
            if (rect) input2->InvalidateRects(rect, 1);
            else input2->Invalidate();
        }

        static float FirstFloat(IEffectGraphValues* values, UINT32 index)
        {
            float v[4]{ NAN, NAN, NAN, NAN };
            if (!values || FAILED(values->GetFloats(index, v, 1))) return NAN;
            return v[0];
        }
        float Committed(UINT32 index)
        {
            winrt::com_ptr<IEffectGraphValues> values;
            return SUCCEEDED(stats2->GetCommittedValues(values.put())) && values ? FirstFloat(values.get(), index) : NAN;
        }
        // A dispatch's values are drawn but not yet read back (S_FALSE with values).
        bool CommitPending()
        {
            winrt::com_ptr<IEffectGraphValues> values;
            return stats2->GetCommittedValues(values.put()) == S_FALSE && values;
        }
        float Landed(UINT32 index)
        {
            winrt::com_ptr<IEffectGraphValues> values;
            return stats2->TryGetValues(values.put()) == S_OK ? FirstFloat(values.get(), index) : NAN;
        }
        uint32_t TexelOf(UINT32 index)
        {
            const auto& fields = Model().FindNode(statsId)->customEffect->analysisFields;
            uint32_t texel = 0;
            for (UINT32 i = 0; i < index && i < fields.size(); ++i) texel += fields[i].pixelCount();
            return texel;
        }
        float Texel(UINT32 index)
        {
            std::vector<float> texels;
            if (FAILED(Evaluator().ReadAnalysisTexture(statsId, texels))) return NAN;
            const size_t at = static_cast<size_t>(TexelOf(index)) * 4;
            return at < texels.size() ? texels[at] : NAN;
        }
        float Bound(const wchar_t* property)
        {
            const auto* node = Model().FindNode(rootId);
            if (!node) return NAN;
            const auto it = node->properties.find(property);
            const float* value = it != node->properties.end() ? std::get_if<float>(&it->second) : nullptr;
            return value ? *value : NAN;
        }
        // Every scalar field's texel holds its committed float, bit for bit.
        bool TexelsMatchRecord()
        {
            std::vector<float> texels;
            winrt::com_ptr<IEffectGraphValues> values;
            if (FAILED(Evaluator().ReadAnalysisTexture(statsId, texels)) ||
                stats2->GetCommittedValues(values.put()) != S_OK)
                return false;
            const auto& fields = Model().FindNode(statsId)->customEffect->analysisFields;
            for (UINT32 i = 0; i < fields.size(); ++i)
            {
                if (fields[i].type != Graph::AnalysisFieldType::Float) continue;
                const size_t at = static_cast<size_t>(TexelOf(i)) * 4;
                if (at >= texels.size() || !SameFloat(texels[at], FirstFloat(values.get(), i))) return false;
            }
            return true;
        }
    };

    UINT32 FieldIndex(const Graph::EffectNode* node, const wchar_t* name)
    {
        const auto& fields = node->customEffect->analysisFields;
        for (UINT32 i = 0; i < fields.size(); ++i)
            if (fields[i].name == name) return i;
        return EFFECTGRAPH_INVALID_INDEX;
    }

    struct RigOptions
    {
        EFFECTGRAPH_COMMIT_POLICY maxPolicy{ EFFECTGRAPH_COMMIT_POLICY_EXACT };
        EFFECTGRAPH_COMMIT_POLICY budgetPolicy{ EFFECTGRAPH_COMMIT_POLICY_EXACT };
        uint32_t size{ 64 };
        bool commandList{ true };
        EFFECTGRAPH_CONTEXT_FLAGS flags{ EFFECTGRAPH_CONTEXT_FLAGS_NONE };
        // The statistics bindings (off: none, for a lane-3 binding added later).
        bool bindStatistics{ true };
        // Always the stand-in tone map: its gpu-bindable indices are known.
        bool standIn{ false };
        // A desktop the replay owns, instead of the rig's own.
        Dda::Desktop* shared{ nullptr };
        // The deferred commit (off: the stage 2 skip-on stage).
        bool deferred{ true };
        bool dirtyRegions{ true };
    };

    DeferredRig MakeDeferredRig(const RigOptions& o)
    {
        DeferredRig rig;
        rig.context = o.deferred ? CreateDeferredContext(o.flags) : CreateStageContext();
        auto options = rig.context->Evaluator().Options();
        options.computeCommandList = o.commandList;
        options.dirtyRegions = o.dirtyRegions;
        options.effectOutputCaching = o.dirtyRegions;
        options.analysisChangeGate = true;
        rig.context->Evaluator().SetOptions(options);
        rig.context2 = rig.context.as<IEffectGraphContext2>();
        ID3D11Texture2D* desktopTexture = nullptr;
        if (o.shared)
        {
            desktopTexture = o.shared->Texture();
            rig.width = o.shared->Width();
            rig.height = o.shared->Height();
        }
        else
        {
            rig.desktop = std::make_unique<Dda::Desktop>(g_d3dDevice.get(), g_d3dContext.get(), o.size, o.size);
            for (uint32_t y = 0; y < o.size; ++y)
                for (uint32_t x = 0; x < o.size; ++x)
                    rig.desktop->Put(x, y, cBackground, cBackground, cBackground);
            rig.desktop->Put(5, 5, cPeak1000, cPeak1000, cPeak1000);
            rig.desktop->UploadAll();
            desktopTexture = rig.desktop->Texture();
            rig.width = rig.height = o.size;
        }

        auto& context = *rig.context;
        bool ok = SUCCEEDED(context.CreateTextureInput(desktopTexture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, rig.input.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Working Space", rig.workingSpace.put())) &&
                  SUCCEEDED(context.CreateEffect(L"Luminance Statistics", rig.stats.put()));
        if (ok && (g_pluginPath.empty() || o.standIn))
        {
            static const auto descriptor = GateToneMapDescriptor();
            const uint32_t id = context.Model().AddNode(Effects::ShaderLabEffects::CreateNode(descriptor));
            ok = context.NodeFor(id, rig.root.put()) == S_OK;
        }
        else if (ok)
        {
            ok = SUCCEEDED(context.CreateEffect(cTonemapName, rig.root.put()));
        }
        ok = ok && SUCCEEDED(rig.stats->SetInput(0, rig.input.get())) &&
             SUCCEEDED(rig.root->SetInput(0, rig.input.get())) &&
             SUCCEEDED(rig.stats->Bind(L"ClipNits", rig.workingSpace.get(), L"SdrWhiteNits")) &&
             SUCCEEDED(rig.root->Bind(L"SdrWhiteNits", rig.workingSpace.get(), L"SdrWhiteNits"));
        if (ok && o.bindStatistics)
            ok = SUCCEEDED(rig.root->Bind(L"SourcePeakNits", rig.stats.get(), L"Max")) &&
                 SUCCEEDED(rig.root->Bind(L"WhiteErrorBudget", rig.stats.get(), L"WhiteErrorBudget"));
        if (!ok) return rig;
        rig.input2 = rig.input.as<IEffectGraphInput2>();
        rig.stats2 = rig.stats.as<IEffectGraphNode2>();
        rig.root2 = rig.root.as<IEffectGraphNode2>();
        if (o.bindStatistics)
            ok = SUCCEEDED(rig.root2->SetCommitPolicy(L"SourcePeakNits", o.maxPolicy)) &&
                 SUCCEEDED(rig.root2->SetCommitPolicy(L"WhiteErrorBudget", o.budgetPolicy));
        rig.target = MakeSrgbTarget(rig.width, rig.height);
        rig.statsId = context.NodeId(rig.stats.get());
        rig.rootId = context.NodeId(rig.root.get());
        rig.inputId = context.NodeId(rig.input.get());
        const auto* statsNode = context.Model().FindNode(rig.statsId);
        rig.maxIndex = FieldIndex(statsNode, L"Max");
        rig.budgetIndex = FieldIndex(statsNode, L"WhiteErrorBudget");
        rig.ok = ok && rig.maxIndex != EFFECTGRAPH_INVALID_INDEX && rig.budgetIndex != EFFECTGRAPH_INVALID_INDEX;
        return rig;
    }

    // A GPU load long enough to keep a readback in flight across the next calls.
    GpuLoad& InFlightLoad()
    {
        static GpuLoad load(1024, 1024, 80.0);
        return load;
    }

    SL_TEST_CASE(Stage3_LagAndDrain, Tag::Gpu)
    {
        printf("\n=== Stage 3: one-frame lag, the drain and S_FALSE ===\n");
        auto rig = MakeDeferredRig({});
        TEST("Stage3Lag_Built", rig.ok);
        if (!rig.ok) return;
        const auto before = CountsOf(rig.Evaluator().ReadbackStats());

        // The first realize schedules the seed's readback.
        const HRESULT first = rig.Realize();
        const HRESULT drained = rig.Drain();
        TEST("Stage3Lag_FirstRealizeLeavesReadback", first == S_FALSE);
        TEST("Stage3Lag_DrainEnds", drained == S_OK);
        TEST("Stage3Lag_Seeded", Near(rig.Committed(rig.maxIndex), 1000.0f) &&
                                 SameFloat(rig.Bound(L"SourcePeakNits"), rig.Committed(rig.maxIndex)) &&
                                 rig.TexelsMatchRecord());

        // Frame N: a rect raises Max. Its readback lands at N + 1.
        const uint64_t changes0 = rig.Changes();
        rig.Paint(5, 5, 6, 6, cPeak2000);
        const HRESULT n = rig.Realize();
        const uint64_t changesN = rig.Changes();
        const bool lagged = Near(rig.Committed(rig.maxIndex), 1000.0f) && Near(rig.Bound(L"SourcePeakNits"), 1000.0f) &&
                            Near(rig.Texel(rig.maxIndex), 1000.0f) && !rig.whole;
        printf("  [info] frame N: hr 0x%08X, committed Max %.2f, texel %.2f, %zu rect(s) drawn\n",
               static_cast<unsigned>(n), rig.Committed(rig.maxIndex), rig.Texel(rig.maxIndex), rig.drawn.size());
        TEST("Stage3Lag_FrameNReturnsFalse", n == S_FALSE);
        TEST("Stage3Lag_FrameNKeepsCommitted", lagged);
        // Frame N + 1, no new rects: the drain commits and draws it, whole.
        const auto& stats = rig.Evaluator().DeferredCommit();
        // One commit's landing distance: the sums' difference across it.
        struct Distance { uint64_t landings, frames; };
        const auto mark = [&] { return Distance{ stats.commitLandings, stats.landingFramesSum }; };
        const auto oneLandingAt = [&](const Distance& before, uint64_t frames)
        {
            return stats.commitLandings == before.landings + 1 && stats.landingFramesSum == before.frames + frames;
        };
        const auto beforeN1 = mark();
        WaitForGpuIdle();
        const HRESULT n1 = rig.Realize();
        printf("  [info] frame N+1: hr 0x%08X, committed Max %.2f, texel %.2f, whole %d\n",
               static_cast<unsigned>(n1), rig.Committed(rig.maxIndex), rig.Texel(rig.maxIndex), rig.whole ? 1 : 0);
        TEST("Stage3Lag_FrameN1Commits", n1 == S_OK && Near(rig.Committed(rig.maxIndex), 2000.0f) &&
                                         SameFloat(rig.Bound(L"SourcePeakNits"), rig.Committed(rig.maxIndex)) &&
                                         rig.TexelsMatchRecord());
        TEST("Stage3Lag_CommitIsOneWholeRect", rig.whole);
        const uint64_t distanceN1 = stats.landingFramesSum - beforeN1.frames;
        TEST("Stage3Lag_FrameN1LandsOneFrameLate", oneLandingAt(beforeN1, 1));
        // OutputChangeCount: once when queued, once when its commit lands.
        TEST("Stage3Lag_ChangeCountQueuedThenCommitted", changesN == changes0 + 1 && rig.Changes() == changesN + 1);
        // Nothing dirty, nothing pending: nothing drawn.
        TEST("Stage3Lag_IdleRealizeDrawsNothing", rig.Realize() == S_OK && rig.drawn.empty());

        // A drain while the copy is still in flight returns S_FALSE again,
        // committing nothing; a later call commits.
        rig.Paint(5, 5, 6, 6, cPeak1000);
        const auto beforeInFlight = mark();
        InFlightLoad().Queue();
        const HRESULT scheduled = rig.Realize();
        const HRESULT stillInFlight = rig.Realize();
        const float whileInFlight = rig.Committed(rig.maxIndex);
        WaitForGpuIdle();
        const HRESULT landed = rig.Realize();
        printf("  [info] in flight: %.1f ms of GPU load; scheduling realize 0x%08X, drain 0x%08X (committed %.2f), "
               "later 0x%08X (committed %.2f)\n", InFlightLoad().Ms(), static_cast<unsigned>(scheduled),
               static_cast<unsigned>(stillInFlight), whileInFlight, static_cast<unsigned>(landed),
               rig.Committed(rig.maxIndex));
        TEST("Stage3Lag_InFlightDrainReturnsFalse", scheduled == S_FALSE && stillInFlight == S_FALSE &&
                                                    Near(whileInFlight, 2000.0f));
        TEST("Stage3Lag_LaterDrainCommits", landed == S_OK && Near(rig.Committed(rig.maxIndex), 1000.0f) && rig.whole);
        // Neither polled first nor drained in time: two frames, which the
        // distance has to read as two, not as the one it reads above.
        const uint64_t distanceInFlight = stats.landingFramesSum - beforeInFlight.frames;
        TEST("Stage3Lag_InFlightLandsTwoFramesLate", oneLandingAt(beforeInFlight, 2));

        // An event-driven host that stops after its last rect: one more call
        // with nothing dirty drains it, though only the readback is pending.
        rig.Paint(5, 5, 6, 6, cPeak3000);
        const HRESULT last = rig.Realize();
        const bool onlyReadbackPending = !rig.Model().HasDirtyNodes() && rig.Evaluator().HasPendingReadbacks();
        WaitForGpuIdle();
        const HRESULT drain = rig.Realize();
        TEST("Stage3Lag_StopAfterLastRectDrains", last == S_FALSE && onlyReadbackPending && drain == S_OK &&
                                                  Near(rig.Committed(rig.maxIndex), 3000.0f) && rig.whole);

        // Poll first: PollReadbacks between frames commits it, counted one
        // frame late (the next realize draws it).
        rig.Paint(5, 5, 6, 6, cPeak2000);
        const HRESULT beforePoll = rig.Realize();
        const auto beforePollMark = mark();
        WaitForGpuIdle();
        rig.context2->PollReadbacks();
        const bool polledOneFrame = oneLandingAt(beforePollMark, 1);
        const uint64_t distancePolled = stats.landingFramesSum - beforePollMark.frames;
        const HRESULT drawPolled = rig.Realize();
        printf("  [info] landing distances: frame N+1 %llu, in flight %llu, poll first %llu frame(s)\n",
               static_cast<unsigned long long>(distanceN1), static_cast<unsigned long long>(distanceInFlight),
               static_cast<unsigned long long>(distancePolled));
        TEST("Stage3Lag_PollFirstLandsOneFrameLate", beforePoll == S_FALSE && polledOneFrame && drawPolled == S_OK &&
             rig.whole && Near(rig.Committed(rig.maxIndex), 2000.0f));
        rig.Paint(5, 5, 6, 6, cPeak3000);
        rig.Realize();
        rig.Drain();

        const auto after = CountsOf(rig.Evaluator().ReadbackStats());
        TEST("Stage3Lag_NoFallbackUnskippedOrFresh", after == before);

        // The opt-in blocking drain: exactly one FreshReadbacks.
        rig.Paint(5, 5, 6, 6, cPeak2000);
        InFlightLoad().Queue();
        const HRESULT pending = rig.Realize();
        const uint64_t fresh0 = rig.Evaluator().ReadbackStats().freshReadbacks;
        const bool committedAtPoll = rig.Evaluator().PollReadbacks(rig.Model(), true);
        const uint64_t fresh = rig.Evaluator().ReadbackStats().freshReadbacks - fresh0;
        const HRESULT drawnAfter = rig.Realize();
        printf("  [info] blocking drain: scheduling realize 0x%08X, %llu fresh readback(s), committed %.2f\n",
               static_cast<unsigned>(pending), static_cast<unsigned long long>(fresh), rig.Committed(rig.maxIndex));
        TEST("Stage3Lag_BlockingDrainOneFresh", pending == S_FALSE && committedAtPoll && fresh == 1 &&
                                                Near(rig.Committed(rig.maxIndex), 2000.0f));
        TEST("Stage3Lag_BlockingDrainDrawnNext", drawnAfter == S_OK && rig.whole);
    }

    SL_TEST_CASE(Stage3_StampRule, Tag::Gpu)
    {
        printf("\n=== Stage 3: the stamp rule ===\n");
        auto rig = MakeDeferredRig({});
        TEST("Stage3Stamp_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const auto& stats = rig.Evaluator().DeferredCommit();

        // A whole dispatch during a 2000-nit transient: drawn at once, and its
        // own slot recorded silently -- no second whole draw, no commit and
        // no change counted while it lands.
        const uint64_t stamps0 = stats.stamps;
        rig.PaintWhole(5, 5, 6, 6, cPeak2000);
        rig.Realize();
        const float wholeTexel = rig.Texel(rig.maxIndex);
        const bool wholeDrew = rig.whole && Near(wholeTexel, 2000.0f);
        // Until its readback lands, the record says the drawn values are pending
        // (S_FALSE with values) and still holds the last commit.
        const bool pendingWhileInFlight = rig.CommitPending() && !Near(rig.Committed(rig.maxIndex), 2000.0f);
        const uint64_t commitsBefore = stats.commitLandings, changesBefore = rig.Changes();
        uint32_t drainWholes = 0;
        rig.Drain(&drainWholes);
        const bool recorded = stats.stamps > stamps0 && Near(rig.Committed(rig.maxIndex), 2000.0f) &&
                              rig.TexelsMatchRecord();
        const bool silent = drainWholes == 0 && stats.commitLandings == commitsBefore && rig.Changes() == changesBefore;
        printf("  [info] stamp: the whole dispatch's texel %.2f; own slot recorded %d; while it landed %u whole draw(s), "
               "%llu commit(s), %llu change(s)\n", wholeTexel, recorded ? 1 : 0, drainWholes,
               static_cast<unsigned long long>(stats.commitLandings - commitsBefore),
               static_cast<unsigned long long>(rig.Changes() - changesBefore));
        TEST("Stage3Stamp_WholeDispatchDrawsAtOnce", wholeDrew);
        TEST("Stage3Stamp_CommittedPendingUntilLanded", pendingWhileInFlight);
        TEST("Stage3Stamp_CommittedSettledAfterLanding", !rig.CommitPending());
        TEST("Stage3Stamp_OwnSlotRecorded", recorded);
        TEST("Stage3Stamp_OwnSlotSilent", silent);

        // Compare on landing, not the stamp rule (the whole slot landed
        // first): a rect that removes the transient is followed.
        rig.Paint(5, 5, 6, 6, cPeak1000);
        rig.Realize();
        rig.Drain();
        printf("  [info] transient: rect back to %.2f (texel %.2f, consumer %.2f)\n", rig.Committed(rig.maxIndex),
               rig.Texel(rig.maxIndex), rig.Bound(L"SourcePeakNits"));
        TEST("Stage3Landing_TransientConsumerFollows", Near(rig.Committed(rig.maxIndex), 1000.0f) &&
                                                       Near(rig.Texel(rig.maxIndex), 1000.0f) &&
                                                       Near(rig.Bound(L"SourcePeakNits"), 1000.0f));

        // Two in flight: Whole at N (2000), Rects at N + 1 back to the
        // committed 1000, one poll at N + 2 delivering the Rects slot.
        const RECT peakRect{ 5, 5, 6, 6 };
        const auto transient = rig.Snapshot(5, 5, 6, 6, cPeak2000);
        const auto restored = rig.Snapshot(5, 5, 6, 6, cPeak1000);
        const uint64_t landed0 = stats.landed;
        InFlightLoad().Queue();
        rig.Show(transient.get(), nullptr);
        rig.Realize();
        rig.Show(restored.get(), &peakRect);
        rig.Realize();
        const bool bothInFlight = stats.landed == landed0;
        WaitForGpuIdle();
        rig.Realize();
        printf("  [info] two in flight (%s): after N+2 committed %.2f, texel %.2f, consumer %.2f\n",
               bothInFlight ? "arranged" : "NOT arranged: the first slot landed early", rig.Committed(rig.maxIndex),
               rig.Texel(rig.maxIndex), rig.Bound(L"SourcePeakNits"));
        TEST("Stage3Stamp_TwoInFlightArranged", bothInFlight);
        TEST("Stage3Stamp_TwoInFlightConsumerRendersR", Near(rig.Committed(rig.maxIndex), 1000.0f) &&
                                                        Near(rig.Texel(rig.maxIndex), 1000.0f) &&
                                                        Near(rig.Bound(L"SourcePeakNits"), 1000.0f));
        rig.Drain();

        // An older Rects slot delivered after a Whole dispatch never reaches
        // the texture: Rects at N (2000) behind one load, Whole at N + 1
        // (3000) behind another; the poll at N + 2 finds only the older slot.
        winrt::com_ptr<ID3D11Query> afterN;
        const D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
        winrt::check_hresult(g_d3dDevice->CreateQuery(&queryDesc, afterN.put()));
        const auto rectFrame = rig.Snapshot(5, 5, 6, 6, cPeak2000);
        const auto wholeFrame = rig.Snapshot(5, 5, 6, 6, cPeak3000);
        const uint64_t discarded0 = stats.discarded, landedBefore = stats.landed;
        InFlightLoad().Queue();
        rig.Show(rectFrame.get(), &peakRect);
        rig.Realize();
        g_d3dContext->End(afterN.get());
        InFlightLoad().Queue();
        rig.Show(wholeFrame.get(), nullptr);
        rig.Realize();
        const bool nothingLandedYet = stats.landed == landedBefore;
        BOOL done = FALSE;
        while (g_d3dContext->GetData(afterN.get(), &done, sizeof(done), 0) == S_FALSE) std::this_thread::yield();
        rig.Realize();
        const bool olderOnly = stats.landed == landedBefore + 1 && stats.discarded == discarded0 + 1;
        const float texelAfterOlder = rig.Texel(rig.maxIndex);
        rig.Drain();
        printf("  [info] older slot (%s): texel after it %.2f; after the whole slot committed %.2f\n",
               nothingLandedYet && olderOnly ? "arranged" : "NOT arranged", texelAfterOlder, rig.Committed(rig.maxIndex));
        TEST("Stage3Stamp_OlderSlotArranged", nothingLandedYet && olderOnly);
        TEST("Stage3Stamp_OlderSlotNeverReachesTexture", Near(texelAfterOlder, 3000.0f));
        TEST("Stage3Stamp_WholeSlotCommits", Near(rig.Committed(rig.maxIndex), 3000.0f) && rig.TexelsMatchRecord());
    }

    void PerFieldPolicies(bool commandList)
    {
        const std::string name = commandList ? "Stage3Fields_CommandList" : "Stage3Fields_Immediate";
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        options.commandList = commandList;
        auto rig = MakeDeferredRig(options);
        TEST((name + "_Built").c_str(), rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const float budget0 = rig.Committed(rig.budgetIndex);
        TEST((name + "_Seeded").c_str(), Near(rig.Committed(rig.maxIndex), 1000.0f) && std::isfinite(budget0) &&
                                         rig.TexelsMatchRecord());

        // Sub-threshold drift: 20 pixels over ClipNits move the budget by
        // about 0.01 dE ITP, inside its 0.05 threshold.
        const uint64_t sub0 = rig.Evaluator().DeferredCommit().subThreshold;
        rig.Paint(10, 40, 30, 41, cClipped);
        rig.Realize();
        rig.Drain();
        const float drifted = rig.Landed(rig.budgetIndex);
        printf("  [info] %s: budget committed %.6f, landed %.6f after the drift\n", name.c_str(), budget0, drifted);
        TEST((name + "_DriftLanded").c_str(), std::isfinite(drifted) && !SameFloat(drifted, budget0) &&
                                             std::fabs(drifted - budget0) <= 0.05f);
        TEST((name + "_SubThresholdNeverReachesTexture").c_str(),
             SameFloat(rig.Committed(rig.budgetIndex), budget0) && SameFloat(rig.Texel(rig.budgetIndex), budget0) &&
             rig.Evaluator().DeferredCommit().subThreshold > sub0);

        // CommitFields on Max: its texel moves, the budget's stays committed
        // with a newer dispatch in the buffer.
        const float host = 1500.0f;
        const HRESULT commit = rig.stats2->CommitFields(&rig.maxIndex, &host, 1);
        const HRESULT realized = rig.Realize();
        TEST((name + "_HostCommitIsOneWholeRect").c_str(), commit == S_OK && realized == S_OK && rig.whole);
        TEST((name + "_HostCommitWritesItsTexel").c_str(),
             SameFloat(rig.Texel(rig.maxIndex), host) && SameFloat(rig.Committed(rig.maxIndex), host) &&
             SameFloat(rig.Bound(L"SourcePeakNits"), host));
        TEST((name + "_HostCommitLeavesOtherTexel").c_str(),
             SameFloat(rig.Texel(rig.budgetIndex), budget0) && SameFloat(rig.Committed(rig.budgetIndex), budget0) &&
             rig.TexelsMatchRecord());

        // A budget commit (400 pixels over ClipNits) with a higher peak in
        // the same rects: the budget commits, Max stays the host's.
        rig.Paint(10, 10, 30, 30, cClipped);
        rig.Paint(5, 5, 6, 6, cPeak2000);
        rig.Realize();
        rig.Drain();
        const float budget1 = rig.Committed(rig.budgetIndex);
        printf("  [info] %s: budget committed %.6f after the block; Max landed %.2f, committed %.2f\n", name.c_str(),
               budget1, rig.Landed(rig.maxIndex), rig.Committed(rig.maxIndex));
        TEST((name + "_ThresholdCommits").c_str(), std::isfinite(budget1) && budget1 > budget0 + 0.05f &&
                                                  SameFloat(rig.Texel(rig.budgetIndex), budget1));
        TEST((name + "_ThresholdCommitLeavesHosted").c_str(),
             SameFloat(rig.Committed(rig.maxIndex), host) && SameFloat(rig.Texel(rig.maxIndex), host) &&
             Near(rig.Landed(rig.maxIndex), 2000.0f) && rig.TexelsMatchRecord());

        // A sub-threshold drift again, so the buffer holds a budget the
        // texture must not: SetFloats writes only the field it names, never
        // a Hosted texel, and never another field's newer buffer value.
        rig.Paint(10, 40, 30, 41, cBackground);
        rig.Realize();
        rig.Drain();
        const bool driftHeld = !SameFloat(rig.Landed(rig.budgetIndex), budget1) &&
                               SameFloat(rig.Committed(rig.budgetIndex), budget1);
        winrt::com_ptr<IEffectGraphValuesBuffer> buffer;
        const float poke = 777.0f, budgetPoke = 0.5f;
        const bool gotBuffer = SUCCEEDED(rig.stats->GetValuesBuffer(buffer.put())) && buffer;
        const bool wroteMax = gotBuffer && SUCCEEDED(buffer->SetFloats(rig.maxIndex, &poke, 1));
        printf("  [info] %s: drift held %d; after SetFloats(Max) the budget texel %.6f (committed %.6f, landed %.6f)\n",
               name.c_str(), driftHeld ? 1 : 0, rig.Texel(rig.budgetIndex), rig.Committed(rig.budgetIndex),
               rig.Landed(rig.budgetIndex));
        TEST((name + "_SetFloatsDriftArranged").c_str(), driftHeld);
        TEST((name + "_SetFloatsLeavesHostedTexel").c_str(),
             wroteMax && SameFloat(rig.Texel(rig.maxIndex), host) && SameFloat(rig.Committed(rig.maxIndex), host));
        TEST((name + "_SetFloatsLeavesOtherFields").c_str(),
             wroteMax && SameFloat(rig.Texel(rig.budgetIndex), budget1) && rig.TexelsMatchRecord());
        const bool wroteBudget = gotBuffer && SUCCEEDED(buffer->SetFloats(rig.budgetIndex, &budgetPoke, 1));
        TEST((name + "_SetFloatsWritesOthers").c_str(), wroteBudget && SameFloat(rig.Texel(rig.budgetIndex), budgetPoke) &&
             SameFloat(rig.Committed(rig.budgetIndex), budgetPoke) && rig.TexelsMatchRecord());
    }

    SL_TEST_CASE(Stage3_PerFieldPolicies, Tag::Gpu)
    {
        printf("\n=== Stage 3: Hosted Max and Threshold WhiteErrorBudget on one producer ===\n");
        // The runner's own deferred context (the WARP path: no driver command
        // lists, where a boxed UpdateSubresource reads the wrong source), and
        // the immediate context.
        PerFieldPolicies(true);
        PerFieldPolicies(false);
    }

    SL_TEST_CASE(Stage3_HostedApi, Tag::Gpu)
    {
        printf("\n=== Stage 3: the Hosted API ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3Hosted_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const auto& stats = rig.Evaluator().DeferredCommit();

        // TryGetValues between realizes changes nothing.
        const uint64_t changes = rig.Changes();
        const float committed = rig.Committed(rig.maxIndex);
        for (int i = 0; i < 3; ++i) rig.Landed(rig.maxIndex);
        TEST("Stage3Hosted_TryGetValuesChangesNothing", rig.Changes() == changes &&
             SameFloat(rig.Committed(rig.maxIndex), committed) && !rig.Model().HasDirtyNodes());

        // A landed value the host has not committed: S_OK, and the realize
        // that landed it does not wait on it.
        rig.Paint(5, 5, 6, 6, cPeak2000);
        rig.Realize();
        const HRESULT drained = rig.Drain();
        winrt::com_ptr<IEffectGraphValues> landed;
        TEST("Stage3Hosted_UncommittedLandedIsOk", rig.stats2->TryGetValues(landed.put()) == S_OK &&
             Near(rig.Landed(rig.maxIndex), 2000.0f) && Near(rig.Committed(rig.maxIndex), 1000.0f));
        TEST("Stage3Hosted_LandedHostedNeverFalse", drained == S_OK && rig.Realize() == S_OK && rig.drawn.empty());

        // A commit is one whole rect in the next drawn.
        const float ratchet = rig.Landed(rig.maxIndex);
        const HRESULT committedHr = rig.stats2->CommitFields(&rig.maxIndex, &ratchet, 1);
        TEST("Stage3Hosted_CommitOneWholeRect", committedHr == S_OK && rig.Realize() == S_OK && rig.whole &&
             SameFloat(rig.Committed(rig.maxIndex), ratchet));
        // Refused: a field that is not Hosted, out of range, non-finite.
        const float nan = NAN;
        const UINT32 outOfRange = 1000;
        TEST("Stage3Hosted_CommitRefusesOtherFields",
             rig.stats2->CommitFields(&rig.budgetIndex, &ratchet, 1) == E_INVALIDARG &&
             rig.stats2->CommitFields(&outOfRange, &ratchet, 1) == E_INVALIDARG &&
             rig.stats2->CommitFields(&rig.maxIndex, &nan, 1) == E_INVALIDARG);

        // After PollReadbacks, the realize's own poll has nothing to commit.
        rig.Paint(10, 10, 30, 30, cClipped);
        rig.Realize();
        WaitForGpuIdle();
        const uint64_t commits0 = stats.commitLandings;
        const HRESULT polled = rig.context2->PollReadbacks();
        const uint64_t commitsAtPoll = stats.commitLandings - commits0;
        const HRESULT afterPoll = rig.Realize();
        TEST("Stage3Hosted_PollCommits", polled == S_OK && commitsAtPoll == 1);
        TEST("Stage3Hosted_RealizeAfterPollCommitsNothing", afterPoll == S_OK &&
             stats.commitLandings - commits0 == 1 && rig.whole);

        // Coincident Threshold and Hosted commits: one whole draw poll first...
        rig.Paint(10, 10, 30, 30, cBackground);
        rig.Paint(5, 5, 6, 6, cPeak3000);
        rig.Realize();
        WaitForGpuIdle();
        uint32_t pollFirstWholes = 0;
        const uint64_t thresholdBeforePoll = stats.thresholdCommits;
        rig.context2->PollReadbacks();
        // The budget committed at the poll, so both commits meet in one realize.
        const bool budgetCommittedAtPoll = stats.thresholdCommits == thresholdBeforePoll + 1;
        const float peak = rig.Landed(rig.maxIndex);
        const uint64_t hostedBefore = stats.hostedCommits, hostedFramesBefore = stats.hostedFramesSum;
        rig.stats2->CommitFields(&rig.maxIndex, &peak, 1);
        if (rig.Realize() == S_OK && rig.whole) ++pollFirstWholes;
        // The host read a value its dispatch's frame made and committed it
        // drawn by the next frame: one frame.
        const uint64_t hostedDistance = stats.hostedFramesSum - hostedFramesBefore;
        TEST("Stage3Hosted_PollFirstCommitOneFrameLate", stats.hostedCommits == hostedBefore + 1 && hostedDistance == 1);
        // A second whole draw would show here: the next realize from an idle
        // GPU has nothing to draw.
        WaitForGpuIdle();
        const bool nothingAfter = rig.Realize() == S_OK && rig.drawn.empty();
        const bool pollFirstCommitted = Near(rig.Committed(rig.maxIndex), 3000.0f) && budgetCommittedAtPoll &&
                                        nothingAfter;
        printf("  [info] coincident, poll first: budget committed at the poll %d, next realize drew nothing %d\n",
               budgetCommittedAtPoll ? 1 : 0, nothingAfter ? 1 : 0);
        // ...two on the drain arm: the realize commits the budget, the host
        // commits after it and drains.
        rig.Paint(10, 10, 30, 30, cClipped);
        rig.Paint(5, 5, 6, 6, cPeak2000);
        rig.Realize();
        WaitForGpuIdle();
        uint32_t drainWholes = 0;
        if (rig.Realize() == S_OK && rig.whole) ++drainWholes;
        const float dropped = rig.Landed(rig.maxIndex);
        rig.stats2->CommitFields(&rig.maxIndex, &dropped, 1);
        if (rig.Realize() == S_OK && rig.whole) ++drainWholes;
        printf("  [info] coincident commits: %u whole draw(s) poll first, %u on the drain arm\n", pollFirstWholes,
               drainWholes);
        TEST("Stage3Hosted_CoincidentPollFirstOneWhole", pollFirstWholes == 1 && pollFirstCommitted);
        TEST("Stage3Hosted_CoincidentDrainArmTwoWholes", drainWholes == 2 && Near(rig.Committed(rig.maxIndex), 2000.0f));

        // A bound ClipNits change on the Hosted producer: one whole forward.
        rig.Realize();
        rig.Drain();
        const float clip = 500.0f;
        const bool reset = SUCCEEDED(rig.stats->Unbind(L"ClipNits")) &&
            SUCCEEDED(rig.stats.as<ID2D1Properties>()->SetValueByName(L"ClipNits", D2D1_PROPERTY_TYPE_FLOAT,
                reinterpret_cast<const BYTE*>(&clip), sizeof(clip)));
        uint32_t wholes = 0;
        const HRESULT clipRealize = rig.Realize();
        if (rig.whole) ++wholes;
        uint32_t drainedWholes = 0;
        if (clipRealize == S_FALSE) rig.Drain(&drainedWholes);
        printf("  [info] ClipNits change: %u whole draw(s), then %u while draining\n", wholes, drainedWholes);
        TEST("Stage3Hosted_ClipNitsOneWholeForward", reset && wholes + drainedWholes == 1 &&
             SameFloat(rig.Committed(rig.maxIndex), dropped) && SameFloat(rig.Texel(rig.maxIndex), dropped) &&
             rig.TexelsMatchRecord());

        // The keep mask: a whole dispatch on the Hosted producer copies every
        // texel but the Hosted one, which keeps the host's value although the
        // dispatch's Max (2000, still on the desktop) differs from it.
        rig.Drain();
        const float held = 1500.0f;
        const HRESULT heldCommit = rig.stats2->CommitFields(&rig.maxIndex, &held, 1);
        rig.Realize();
        const float budgetBefore = rig.Committed(rig.budgetIndex);
        // 1000 nits: over the 500-nit ClipNits set above, so the budget moves.
        rig.PaintWhole(10, 10, 30, 30, cPeak1000);
        rig.Realize();
        const float maxTexelAfterWhole = rig.Texel(rig.maxIndex);
        const float budgetTexelAfterWhole = rig.Texel(rig.budgetIndex);
        rig.Drain();
        printf("  [info] keep mask: host Max %.2f; after the whole dispatch Max texel %.2f (landed %.2f), budget texel "
               "%.6f (was %.6f, landed %.6f)\n", held, maxTexelAfterWhole, rig.Landed(rig.maxIndex),
               budgetTexelAfterWhole, budgetBefore, rig.Landed(rig.budgetIndex));
        TEST("Stage3Hosted_WholeDispatchArranged", heldCommit == S_OK && Near(rig.Landed(rig.maxIndex), 2000.0f) &&
             !SameFloat(rig.Landed(rig.budgetIndex), budgetBefore));
        TEST("Stage3Hosted_WholeDispatchKeepsHostedTexel", SameFloat(maxTexelAfterWhole, held) &&
             SameFloat(rig.Texel(rig.maxIndex), held) && SameFloat(rig.Committed(rig.maxIndex), held) &&
             SameFloat(rig.Bound(L"SourcePeakNits"), held));
        TEST("Stage3Hosted_WholeDispatchCopiesOthers", SameFloat(budgetTexelAfterWhole, rig.Landed(rig.budgetIndex)) &&
             SameFloat(rig.Committed(rig.budgetIndex), rig.Landed(rig.budgetIndex)) && rig.TexelsMatchRecord());
    }

    SL_TEST_CASE(Stage3_Seeding, Tag::Gpu)
    {
        printf("\n=== Stage 3: seeding the record ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        options.flags = EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK;
        {
            auto rig = MakeDeferredRig(options);
            TEST("Stage3Seed_Built", rig.ok);
            if (!rig.ok) return;
            const auto before = rig.Evaluator().ReadbackStats().fallbackReadbacks;
            winrt::com_ptr<IEffectGraphValues> none;
            const bool unseeded = rig.stats2->GetCommittedValues(none.put()) == S_FALSE && !none;
            const HRESULT first = rig.Realize();
            const float texel = rig.Texel(rig.maxIndex);
            rig.Drain();
            printf("  [info] NO_FALLBACK first realize 0x%08X, Hosted texel %.2f, committed %.2f\n",
                   static_cast<unsigned>(first), texel, rig.Committed(rig.maxIndex));
            TEST("Stage3Seed_NoFallbackFirstRealize", SUCCEEDED(first) &&
                 rig.Evaluator().ReadbackStats().fallbackReadbacks == before);
            TEST("Stage3Seed_CommittedFalseBeforeSeed", unseeded);
            TEST("Stage3Seed_HostedTexelFromFirstDispatch", Near(texel, 1000.0f) &&
                 SameFloat(rig.Committed(rig.maxIndex), texel) && rig.TexelsMatchRecord());

            // Device loss (the evaluator's cache released, as a move to a new
            // device does): the next dispatch seeds again, Hosted included.
            rig.Paint(5, 5, 6, 6, cPeak2000);
            rig.Evaluator().ReleaseCache(rig.Model());
            const HRESULT again = rig.Realize();
            const float texelAgain = rig.Texel(rig.maxIndex);
            rig.Drain();
            printf("  [info] after device loss: realize 0x%08X, Hosted texel %.2f, committed %.2f\n",
                   static_cast<unsigned>(again), texelAgain, rig.Committed(rig.maxIndex));
            TEST("Stage3Seed_AfterDeviceLoss", SUCCEEDED(again) && Near(texelAgain, 2000.0f) &&
                 Near(rig.Committed(rig.maxIndex), 2000.0f) && rig.TexelsMatchRecord() &&
                 rig.Evaluator().ReadbackStats().fallbackReadbacks == before);
        }
        {
            // The first fill's slot and a Rects slot delivered by one poll:
            // the Rects slot seeds every field, the Hosted texel included.
            auto rig = MakeDeferredRig(options);
            if (!rig.ok) return;
            const RECT peakRect{ 5, 5, 6, 6 };
            const auto raised = rig.Snapshot(5, 5, 6, 6, cPeak2000);
            InFlightLoad().Queue();
            rig.Realize();
            rig.Show(raised.get(), &peakRect);
            rig.Realize();
            winrt::com_ptr<IEffectGraphValues> none;
            const bool notYet = rig.stats2->GetCommittedValues(none.put()) == S_FALSE && !none;
            WaitForGpuIdle();
            rig.Realize();
            winrt::com_ptr<IEffectGraphValues> landed;
            printf("  [info] one poll, two slots (%s): committed Max %.2f, texel %.2f\n",
                   notYet ? "arranged" : "NOT arranged", rig.Committed(rig.maxIndex), rig.Texel(rig.maxIndex));
            TEST("Stage3Seed_TwoSlotsArranged", notYet);
            TEST("Stage3Seed_TwoSlotsSeed", rig.stats2->TryGetValues(landed.put()) == S_OK &&
                 Near(rig.Committed(rig.maxIndex), 2000.0f) && Near(rig.Texel(rig.maxIndex), 2000.0f) &&
                 rig.TexelsMatchRecord() && SameFloat(rig.Bound(L"SourcePeakNits"), rig.Committed(rig.maxIndex)));
        }
        {
            // The first fill's slot delivered alone after a whole dispatch
            // (the stamp) went out: it does not seed, since the texture holds
            // the whole dispatch's values; the stamp's own slot seeds later.
            auto rig = MakeDeferredRig(options);
            if (!rig.ok) return;
            const auto& stats = rig.Evaluator().DeferredCommit();
            winrt::com_ptr<ID3D11Query> afterFill;
            const D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
            winrt::check_hresult(g_d3dDevice->CreateQuery(&queryDesc, afterFill.put()));
            const auto wholeFrame = rig.Snapshot(5, 5, 6, 6, cPeak3000);
            const uint64_t landed0 = stats.landed, discarded0 = stats.discarded;
            InFlightLoad().Queue();
            rig.Realize();
            g_d3dContext->End(afterFill.get());
            InFlightLoad().Queue();
            rig.Show(wholeFrame.get(), nullptr);
            rig.Realize();
            const bool nothingLandedYet = stats.landed == landed0;
            BOOL done = FALSE;
            while (g_d3dContext->GetData(afterFill.get(), &done, sizeof(done), 0) == S_FALSE) std::this_thread::yield();
            rig.Realize();
            winrt::com_ptr<IEffectGraphValues> none;
            const bool fillOnly = stats.landed == landed0 + 1 && stats.discarded == discarded0 + 1;
            const bool unseeded = rig.stats2->GetCommittedValues(none.put()) == S_FALSE && !none;
            rig.Drain();
            printf("  [info] fill slot behind a stamp (%s): unseeded after it %d; then committed Max %.2f, texel %.2f\n",
                   nothingLandedYet && fillOnly ? "arranged" : "NOT arranged", unseeded ? 1 : 0,
                   rig.Committed(rig.maxIndex), rig.Texel(rig.maxIndex));
            TEST("Stage3Seed_OlderThanStampArranged", nothingLandedYet && fillOnly);
            TEST("Stage3Seed_OlderThanStampDoesNotSeed", unseeded);
            TEST("Stage3Seed_StampSlotSeeds", Near(rig.Committed(rig.maxIndex), 3000.0f) &&
                 Near(rig.Texel(rig.maxIndex), 3000.0f) && rig.TexelsMatchRecord() &&
                 SameFloat(rig.Bound(L"SourcePeakNits"), rig.Committed(rig.maxIndex)));
        }
        {
            // A CommitFields before the seed survives it, in record and texel.
            auto rig = MakeDeferredRig(options);
            if (!rig.ok) return;
            const float host = 1234.0f;
            const HRESULT commit = rig.stats2->CommitFields(&rig.maxIndex, &host, 1);
            rig.Realize();
            rig.Drain();
            TEST("Stage3Seed_EarlyCommitSurvives", commit == S_OK && SameFloat(rig.Committed(rig.maxIndex), host) &&
                 SameFloat(rig.Texel(rig.maxIndex), host) && SameFloat(rig.Bound(L"SourcePeakNits"), host) &&
                 rig.TexelsMatchRecord());

            // The host owned the field only until the seed: under EXACT the
            // engine judges it again, at once and on later content.
            const HRESULT exact = rig.root2->SetCommitPolicy(L"SourcePeakNits", EFFECTGRAPH_COMMIT_POLICY_EXACT);
            rig.Realize();
            rig.Drain();
            const float afterPolicy = rig.Committed(rig.maxIndex);
            rig.Paint(5, 5, 6, 6, cPeak2000);
            rig.Realize();
            rig.Drain();
            printf("  [info] early commit, then EXACT: committed Max %.2f after the policy change, %.2f after a 2000-nit "
                   "rect\n", afterPolicy, rig.Committed(rig.maxIndex));
            TEST("Stage3Seed_PolicyChangeJudgedAtOnce", exact == S_OK && Near(afterPolicy, 1000.0f));
            TEST("Stage3Seed_EarlyCommitNotOwnedAfterSeed", Near(rig.Committed(rig.maxIndex), 2000.0f) &&
                 Near(rig.Texel(rig.maxIndex), 2000.0f) && rig.TexelsMatchRecord());
        }
        {
            // Seeded while no consumer wanted the texture (GPU bindings off:
            // the consumer reads the record as a constant), committed by the
            // host, then the texture's first fill: the Hosted texel takes the
            // committed record, not the dispatch's value.
            auto rig = MakeDeferredRig(options);
            if (!rig.ok) return;
            auto off = rig.Evaluator().Options();
            off.gpuBindings = false;
            rig.Evaluator().SetOptions(off);
            rig.Realize();
            rig.Drain();
            std::vector<float> none;
            const bool noTexture = FAILED(rig.Evaluator().ReadAnalysisTexture(rig.statsId, none));
            const float host = 1500.0f;
            const HRESULT commit = rig.stats2->CommitFields(&rig.maxIndex, &host, 1);
            rig.Realize();
            rig.Paint(5, 5, 6, 6, cPeak2000);
            rig.Realize();
            rig.Drain();
            const bool arranged = noTexture && commit == S_OK && SameFloat(rig.Committed(rig.maxIndex), host) &&
                                  Near(rig.Landed(rig.maxIndex), 2000.0f);
            auto on = off;
            on.gpuBindings = true;
            rig.Evaluator().SetOptions(on);
            rig.Realize();
            rig.Drain();
            const float texel = rig.Texel(rig.maxIndex);
            printf("  [info] first fill after the seed (%s): Hosted texel %.2f, committed %.2f, landed %.2f\n",
                   arranged ? "arranged" : "NOT arranged", texel, rig.Committed(rig.maxIndex), rig.Landed(rig.maxIndex));
            TEST("Stage3Seed_FirstFillAfterSeedArranged", arranged);
            TEST("Stage3Seed_FirstFillAfterSeedKeepsCommitted", SameFloat(texel, host) &&
                 SameFloat(rig.Committed(rig.maxIndex), host) && rig.TexelsMatchRecord());
            // A later commit moves it; the same value again holds.
            const float next = 1700.0f;
            rig.stats2->CommitFields(&rig.maxIndex, &next, 1);
            rig.Realize();
            rig.stats2->CommitFields(&rig.maxIndex, &next, 1);
            rig.Realize();
            TEST("Stage3Seed_FirstFillAfterSeedCommitsLater", SameFloat(rig.Texel(rig.maxIndex), next) &&
                 SameFloat(rig.Committed(rig.maxIndex), next) && rig.TexelsMatchRecord());
        }
    }

    // A copy pass that cannot run (its masked shader or keep buffer not
    // created): the landing finds the record unchanged and writes the texels
    // of the fields that moved.
    SL_TEST_CASE(Stage3_CopyFailure, Tag::Gpu)
    {
        printf("\n=== Stage 3: a failed copy pass ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3CopyFail_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const auto& stats = rig.Evaluator().DeferredCommit();
        const float budget0 = rig.Committed(rig.budgetIndex);
        const float max0 = rig.Committed(rig.maxIndex);
        const uint64_t failures0 = stats.copyFailures;

        // A whole dispatch (masked: Max is Hosted) whose copy does not run,
        // over a block that moves the budget past its threshold.
        rig.Fill(10, 10, 30, 30, cClipped);
        rig.Evaluator().FailNextTextureCopyForTest(rig.statsId);
        rig.PaintWhole(5, 5, 6, 6, cPeak1000);
        rig.Realize();
        const float texelAfterDispatch = rig.Texel(rig.budgetIndex);
        uint32_t wholes = 0;
        rig.Drain(&wholes);
        printf("  [info] failed copy: %llu failure(s); budget texel %.6f after the dispatch, committed %.6f -> %.6f, "
               "%u whole draw(s) draining\n", static_cast<unsigned long long>(stats.copyFailures - failures0),
               texelAfterDispatch, budget0, rig.Committed(rig.budgetIndex), wholes);
        TEST("Stage3CopyFail_Counted", stats.copyFailures == failures0 + 1);
        TEST("Stage3CopyFail_TextureUntouched", SameFloat(texelAfterDispatch, budget0));
        TEST("Stage3CopyFail_LandingWritesTexel", rig.Committed(rig.budgetIndex) > budget0 + 0.05f &&
             SameFloat(rig.Texel(rig.budgetIndex), rig.Committed(rig.budgetIndex)) &&
             SameFloat(rig.Committed(rig.maxIndex), max0) && rig.TexelsMatchRecord() && wholes == 1);
    }

    // A host write (SetFloats) with an older dispatch's readback still in
    // flight: the landing, the seed included, leaves the written field alone;
    // only a later dispatch replaces it, as with the commit off.
    SL_TEST_CASE(Stage3_HostWriteOrdering, Tag::Gpu)
    {
        printf("\n=== Stage 3: a host write against readbacks in flight ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        const float poke = 5.0f;
        {
            // Before the seed: the first fill's readback held in flight.
            auto rig = MakeDeferredRig(options);
            TEST("Stage3HostWrite_Built", rig.ok);
            if (!rig.ok) return;
            const auto& stats = rig.Evaluator().DeferredCommit();
            InFlightLoad().Queue();
            const HRESULT first = rig.Realize();
            winrt::com_ptr<IEffectGraphValues> none;
            const bool unseeded = first == S_FALSE && rig.stats2->GetCommittedValues(none.put()) == S_FALSE && !none;
            winrt::com_ptr<IEffectGraphValuesBuffer> buffer;
            const bool wrote = SUCCEEDED(rig.stats->GetValuesBuffer(buffer.put())) && buffer &&
                               SUCCEEDED(buffer->SetFloats(rig.budgetIndex, &poke, 1));
            winrt::com_ptr<IEffectGraphValues> stillNone;
            const bool stillUnseeded = rig.stats2->GetCommittedValues(stillNone.put()) == S_FALSE && !stillNone && stats.seeds == 0;
            const float texelAtWrite = rig.Texel(rig.budgetIndex);
            const uint64_t kept0 = stats.hostWritesKept;
            rig.Drain();
            printf("  [info] before the seed: texel at the write %.4f; after the seed budget %.4f (texel %.4f), Max %.2f\n",
                   texelAtWrite, rig.Committed(rig.budgetIndex), rig.Texel(rig.budgetIndex), rig.Committed(rig.maxIndex));
            TEST("Stage3HostWrite_BeforeSeedArranged", unseeded && wrote && stillUnseeded);
            TEST("Stage3HostWrite_BeforeSeedWritesTexel", SameFloat(texelAtWrite, poke));
            TEST("Stage3HostWrite_SeedKeepsWrite", stats.hostWritesKept > kept0 &&
                 SameFloat(rig.Committed(rig.budgetIndex), poke) && SameFloat(rig.Texel(rig.budgetIndex), poke) &&
                 Near(rig.Committed(rig.maxIndex), 1000.0f) && rig.TexelsMatchRecord());
        }
        {
            // After the seed: a rects dispatch's readback held in flight.
            auto rig = MakeDeferredRig(options);
            if (!rig.ok) return;
            const auto& stats = rig.Evaluator().DeferredCommit();
            rig.Realize();
            rig.Drain();
            const float budget0 = rig.Committed(rig.budgetIndex);
            winrt::com_ptr<IEffectGraphValuesBuffer> buffer;
            const bool gotBuffer = SUCCEEDED(rig.stats->GetValuesBuffer(buffer.put())) && buffer;
            InFlightLoad().Queue();
            rig.Paint(10, 10, 30, 30, cClipped);
            const HRESULT scheduled = rig.Realize();
            const uint64_t landed0 = stats.landed, kept0 = stats.hostWritesKept;
            const bool wrote = gotBuffer && SUCCEEDED(buffer->SetFloats(rig.budgetIndex, &poke, 1));
            const bool inFlight = scheduled == S_FALSE && stats.landed == landed0;
            WaitForGpuIdle();
            rig.Realize();
            const bool olderLanded = stats.landed == landed0 + 1;
            const float landedBudget = rig.Landed(rig.budgetIndex);
            printf("  [info] after the seed: budget %.6f, the older slot landed %.6f; record %.4f, texel %.4f\n",
                   budget0, landedBudget, rig.Committed(rig.budgetIndex), rig.Texel(rig.budgetIndex));
            TEST("Stage3HostWrite_AfterSeedArranged", wrote && inFlight && olderLanded && landedBudget > budget0 + 0.05f);
            TEST("Stage3HostWrite_OlderSlotKeepsWrite", stats.hostWritesKept > kept0 &&
                 SameFloat(rig.Committed(rig.budgetIndex), poke) && SameFloat(rig.Texel(rig.budgetIndex), poke) &&
                 rig.TexelsMatchRecord());
            // A dispatch after the write replaces it.
            rig.Paint(40, 40, 41, 41, cBackground);
            rig.Realize();
            rig.Drain();
            TEST("Stage3HostWrite_LaterDispatchReplacesWrite", !SameFloat(rig.Committed(rig.budgetIndex), poke) &&
                 SameFloat(rig.Committed(rig.budgetIndex), rig.Landed(rig.budgetIndex)) && rig.TexelsMatchRecord());
        }
    }

    // A deferred slot whose Map fails for good: the node says so, and a whole
    // dispatch judges the values again, instead of dropping them.
    SL_TEST_CASE(Stage3_ReadbackFailure, Tag::Gpu)
    {
        printf("\n=== Stage 3: a failed readback Map ===\n");
        auto rig = MakeDeferredRig({});
        TEST("Stage3MapFail_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const auto& stats = rig.Evaluator().DeferredCommit();
        const uint64_t failures0 = stats.readbackFailures;

        rig.Paint(5, 5, 6, 6, cPeak2000);
        rig.Evaluator().FailNextReadbackPollForTest(rig.statsId);
        InFlightLoad().Queue();
        const HRESULT scheduled = rig.Realize();
        WaitForGpuIdle();
        rig.context2->PollReadbacks();
        const auto* stats0 = rig.Model().FindNode(rig.statsId);
        const std::wstring error = stats0 ? stats0->runtimeError : std::wstring{};
        const bool redispatchQueued = stats0 && stats0->IsDirty();
        const float committedAfterFailure = rig.Committed(rig.maxIndex);
        const uint64_t whole0 = stats.wholeDispatches;
        const HRESULT again = rig.Realize();
        const bool drewWhole = rig.whole;
        const bool wholeDispatch = stats.wholeDispatches == whole0 + 1;
        rig.Drain();
        const auto* statsAfter = rig.Model().FindNode(rig.statsId);
        printf("  [info] Map failure: %llu failure(s), error '%ls'; redispatch %d whole %d; committed %.2f -> %.2f\n",
               static_cast<unsigned long long>(stats.readbackFailures - failures0), error.c_str(),
               wholeDispatch ? 1 : 0, drewWhole ? 1 : 0, committedAfterFailure, rig.Committed(rig.maxIndex));
        TEST("Stage3MapFail_Arranged", scheduled == S_FALSE && stats.readbackFailures == failures0 + 1);
        TEST("Stage3MapFail_ReportedOnTheNode", error.find(L"readback failed") != std::wstring::npos);
        TEST("Stage3MapFail_WholeRedispatch", redispatchQueued && again == S_FALSE && wholeDispatch && drewWhole);
        TEST("Stage3MapFail_ValuesJudgedAgain", Near(committedAfterFailure, 1000.0f) &&
             Near(rig.Committed(rig.maxIndex), 2000.0f) && rig.TexelsMatchRecord() &&
             Near(rig.Bound(L"SourcePeakNits"), 2000.0f) && statsAfter && statsAfter->runtimeError.empty());
    }

    // Two bindings of one field that disagree on its policy: every consumer
    // of the field fails to resolve, a built-in one too, and the field
    // commits as EXACT until they agree.
    SL_TEST_CASE(Stage3_PolicyConflict, Tag::Gpu)
    {
        printf("\n=== Stage 3: bindings that disagree on a field's policy ===\n");
        RigOptions options;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3Conflict_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const auto& stats = rig.Evaluator().DeferredCommit();

        // Control: one binding, so the Hosted budget takes a commit.
        const float hostBudget = 0.25f;
        const HRESULT control = rig.stats2->CommitFields(&rig.budgetIndex, &hostBudget, 1);
        rig.Realize();
        const bool controlOk = control == S_OK && SameFloat(rig.Committed(rig.budgetIndex), hostBudget);

        // A built-in consumer binds the same field under EXACT.
        winrt::com_ptr<IEffectGraphNode> blur;
        const bool added = SUCCEEDED(rig.context->CreateEffect(L"Gaussian Blur", blur.put())) &&
                           SUCCEEDED(blur->SetInput(0, rig.input.get())) &&
                           SUCCEEDED(blur->Bind(L"StandardDeviation", rig.stats.get(), L"WhiteErrorBudget"));
        TEST("Stage3Conflict_Arranged", controlOk && added);
        if (!added) return;
        const uint32_t blurId = rig.context->NodeId(blur.get());
        const auto realize = [&](IEffectGraphNode* root)
        {
            RECT rects[16]{};
            UINT32 count = 0;
            return rig.context2->RealizeToIncremental(root, rig.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL,
                                                      rects, 16, &count, nullptr, 0, nullptr);
        };
        const auto drain = [&](IEffectGraphNode* root)
        {
            HRESULT hr = S_FALSE;
            for (int i = 0; i < 16; ++i)
            {
                WaitForGpuIdle();
                hr = realize(root);
                if (!rig.Evaluator().HasPendingReadbacks()) break;
            }
            return hr;
        };
        const auto errorOf = [&](uint32_t id)
        {
            const auto* node = rig.Model().FindNode(id);
            return node ? node->runtimeError : std::wstring{};
        };
        const HRESULT toneMapHr = drain(rig.root.get());
        const std::wstring toneMapError = errorOf(rig.rootId);
        const HRESULT blurHr = drain(blur.get());
        const std::wstring blurError = errorOf(blurId);
        printf("  [info] conflict: tone map 0x%08X '%ls'; blur 0x%08X '%ls'\n", static_cast<unsigned>(toneMapHr),
               toneMapError.c_str(), static_cast<unsigned>(blurHr), blurError.c_str());
        TEST("Stage3Conflict_CustomConsumerFails", toneMapHr == EFFECTGRAPH_E_NODE_FAILED &&
             toneMapError.find(L"Commit policy conflict") == 0);
        TEST("Stage3Conflict_BuiltInConsumerFails", blurHr == EFFECTGRAPH_E_NODE_FAILED &&
             blurError.find(L"Commit policy conflict") == 0);
        TEST("Stage3Conflict_CommitFieldsRefused",
             rig.stats2->CommitFields(&rig.budgetIndex, &hostBudget, 1) == E_INVALIDARG);

        // Committed as EXACT: a budget change lands as an engine commit.
        const uint64_t exact0 = stats.exactCommits, hosted0 = stats.hostedCommits, threshold0 = stats.thresholdCommits;
        rig.Paint(10, 10, 30, 30, cClipped);
        drain(blur.get());
        printf("  [info] conflicted budget: %llu Exact commit(s), %llu Hosted, %llu Threshold; committed %.6f, landed %.6f\n",
               static_cast<unsigned long long>(stats.exactCommits - exact0),
               static_cast<unsigned long long>(stats.hostedCommits - hosted0),
               static_cast<unsigned long long>(stats.thresholdCommits - threshold0), rig.Committed(rig.budgetIndex),
               rig.Landed(rig.budgetIndex));
        TEST("Stage3Conflict_CommitsAsExact", stats.exactCommits > exact0 && stats.hostedCommits == hosted0 &&
             stats.thresholdCommits == threshold0 && !SameFloat(rig.Committed(rig.budgetIndex), hostBudget) &&
             SameFloat(rig.Committed(rig.budgetIndex), rig.Landed(rig.budgetIndex)));

        // They agree again: both consumers render, with no error.
        const bool agreed = SUCCEEDED(blur.as<IEffectGraphNode2>()->SetCommitPolicy(L"StandardDeviation",
                                                                                   EFFECTGRAPH_COMMIT_POLICY_HOSTED));
        const HRESULT blurAfter = drain(blur.get());
        const HRESULT toneMapAfter = drain(rig.root.get());
        TEST("Stage3Conflict_ResolvedRendersAgain", agreed && SUCCEEDED(blurAfter) && SUCCEEDED(toneMapAfter) &&
             errorOf(blurId).empty() && errorOf(rig.rootId).empty() &&
             rig.stats2->CommitFields(&rig.budgetIndex, &hostBudget, 1) == S_OK);
    }

    // The drain arm (realize first, decide after): a CommitFields after
    // realize N + 1 and one more realize reach the next acquire as one whole
    // rect, its surface the forced-whole render of the committed values.
    SL_TEST_CASE(Stage3_DrainArmCommit, Tag::Gpu)
    {
        printf("\n=== Stage 3: a host commit on the drain arm ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3DrainArm_Built", rig.ok);
        if (!rig.ok) return;
        RigOptions referenceOptions;
        referenceOptions.deferred = false;
        referenceOptions.dirtyRegions = false;
        referenceOptions.bindStatistics = false;
        referenceOptions.shared = rig.desktop.get();
        auto reference = MakeDeferredRig(referenceOptions);
        TEST("Stage3DrainArm_ReferenceBuilt", reference.ok);
        if (!reference.ok) return;

        // The base driver commits nothing: the host's commit below is the one.
        Dda::StageSim stage(rig.context.get(), rig.input.get(), rig.root.get(), rig.target.get(), g_d3dContext.get());
        stage.SetPollFirst(false);
        auto client = MakeClientSurface(rig.width, rig.height);
        const uint32_t clientIndex = stage.AddClient(client.get());
        stage.Prime();
        for (int i = 0; i < 8 && stage.TickDue(); ++i)
        {
            WaitForGpuIdle();
            stage.Tick();
        }
        stage.Acquire(clientIndex);

        // Realize N raises Max in a rect; realize N + 1 lands it, which for a
        // Hosted field draws nothing.
        Dda::DesktopFrame frame;
        rig.Fill(5, 5, 6, 6, cPeak2000);
        frame.dirty = { RECT{ 5, 5, 6, 6 } };
        const HRESULT n = stage.Present(frame);
        const auto rectsDelivery = stage.Acquire(clientIndex);
        WaitForGpuIdle();
        const HRESULT n1 = stage.Tick();
        const bool n1DrewNothing = n1 == S_OK && stage.WholeDraws() == 0 && Near(rig.Landed(rig.maxIndex), 2000.0f) &&
                                   Near(rig.Committed(rig.maxIndex), 1000.0f);
        // The host commits after N + 1; one drain realize draws it.
        const float landed = rig.Landed(rig.maxIndex);
        const HRESULT commit = rig.stats2->CommitFields(&rig.maxIndex, &landed, 1);
        const HRESULT drained = stage.Tick();
        const uint32_t realizes = stage.Realizes(), wholes = stage.WholeDraws();
        const auto delivery = stage.Acquire(clientIndex);
        // The forced-whole render of the values the stage committed.
        auto referenceProps = reference.root.as<ID2D1Properties>();
        const float max = rig.Committed(rig.maxIndex), budget = rig.Committed(rig.budgetIndex);
        const bool set = SUCCEEDED(referenceProps->SetValueByName(L"SourcePeakNits", D2D1_PROPERTY_TYPE_FLOAT,
                             reinterpret_cast<const BYTE*>(&max), sizeof(max))) &&
                         SUCCEEDED(referenceProps->SetValueByName(L"WhiteErrorBudget", D2D1_PROPERTY_TYPE_FLOAT,
                             reinterpret_cast<const BYTE*>(&budget), sizeof(budget)));
        reference.input2->Invalidate();
        const bool rendered = set && SUCCEEDED(reference.Realize());
        const auto diff = Dda::Compare(Dda::ReadBytes(client.get()), Dda::ReadBytes(reference.target.get()), rig.width, {});
        const bool oneWholeRect = delivery.rects.size() == 1 && delivery.rects[0].left == 0 && delivery.rects[0].top == 0 &&
                                  delivery.rects[0].right == static_cast<LONG>(rig.width) &&
                                  delivery.rects[0].bottom == static_cast<LONG>(rig.height);
        printf("  [info] drain arm: N 0x%08X (%zu rect(s) delivered), N+1 drew nothing %d, commit 0x%08X, drain realize "
               "0x%08X (%u realize(s), %u whole); delivered whole %d, %zu rect(s); %zu pixel(s) differ from the "
               "forced-whole render\n", static_cast<unsigned>(n), rectsDelivery.rects.size(), n1DrewNothing ? 1 : 0,
               static_cast<unsigned>(commit), static_cast<unsigned>(drained), realizes, wholes, delivery.whole ? 1 : 0,
               delivery.rects.size(), diff.differing);
        TEST("Stage3DrainArm_Arranged", n == S_FALSE && rectsDelivery.hr == S_OK && !rectsDelivery.whole &&
             n1DrewNothing && commit == S_OK);
        TEST("Stage3DrainArm_CommitIsOneWholeRect", drained == S_OK && realizes == 1 && wholes == 1 &&
             delivery.hr == S_OK && delivery.whole && oneWholeRect);
        TEST("Stage3DrainArm_SurfaceEqualsForcedWhole", rendered && diff.differing == 0 && SameFloat(max, landed));
    }

    SL_TEST_CASE(Stage3_Lane3BindingOnCleanProducer, Tag::Gpu)
    {
        printf("\n=== Stage 3: a lane-3 binding added to a clean producer ===\n");
        RigOptions options;
        options.bindStatistics = false;
        options.standIn = true;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3Lane3_Built", rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        // The statistics evaluated once with nothing bound to them: clean,
        // with no texture ever requested.
        winrt::com_ptr<IEffectGraphValues> values;
        const bool evaluated = SUCCEEDED(rig.stats->GetValues(values.put()));
        std::vector<float> texels;
        const bool noTexture = FAILED(rig.Evaluator().ReadAnalysisTexture(rig.statsId, texels));
        const auto& deferredStats = rig.Evaluator().DeferredCommit();
        const uint64_t fallback0 = rig.Evaluator().ReadbackStats().fallbackReadbacks;
        const uint64_t whole0 = deferredStats.wholeDispatches;
        const bool bound = SUCCEEDED(rig.root->Bind(L"SourcePeakNits", rig.stats.get(), L"Max"));
        const bool clean = !rig.Model().FindNode(rig.statsId)->IsDirty();
        const HRESULT hr = rig.Realize();
        const uint32_t bits = rig.Evaluator().AppliedGpuBits(rig.rootId);
        const uint64_t wholeDispatches = deferredStats.wholeDispatches - whole0;
        rig.Drain();
        printf("  [info] after the binding: %llu whole dispatch(es), consumer bits 0x%X, %llu fallback readback(s)\n",
               static_cast<unsigned long long>(wholeDispatches), bits,
               static_cast<unsigned long long>(rig.Evaluator().ReadbackStats().fallbackReadbacks - fallback0));
        TEST("Stage3Lane3_ProducerWasClean", evaluated && noTexture && bound && clean);
        TEST("Stage3Lane3_OneWholeDispatch", wholeDispatches == 1);
        TEST("Stage3Lane3_ServedOnTheGpuThatFrame", SUCCEEDED(hr) && (bits & 3u) == 2u);
        TEST("Stage3Lane3_NoFallbackReadback", rig.Evaluator().ReadbackStats().fallbackReadbacks == fallback0);
        TEST("Stage3Lane3_ConsumerHasTheValue", Near(rig.Bound(L"SourcePeakNits"), 1000.0f));
    }

    // The kill switch: without the context flag, or with the readback skip
    // off, Hosted and Threshold bindings behave as Immediate: every dispatch's
    // values reach the texture the consumers read, in the realize that made them.
    SL_TEST_CASE(Stage3_KillSwitch, Tag::Gpu)
    {
        printf("\n=== Stage 3: the kill switch ===\n");
        const auto check = [](const char* name, bool deferredFlag)
        {
            RigOptions options;
            options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
            options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
            options.deferred = deferredFlag;
            auto rig = MakeDeferredRig(options);
            TEST((std::string(name) + "_Built").c_str(), rig.ok);
            if (!rig.ok) return;
            if (deferredFlag)
            {
                // The flag without the skip: no deferred commit.
                auto skipOff = rig.Evaluator().Options();
                skipOff.skipUnneededCpuReadback = false;
                rig.Evaluator().SetOptions(skipOff);
            }
            const HRESULT first = rig.Realize();
            const float budget0 = rig.Texel(rig.budgetIndex);
            const auto stats0 = rig.Evaluator().DeferredCommit();
            // A 2000-nit peak and a sub-threshold budget drift, in rects.
            rig.Paint(5, 5, 6, 6, cPeak2000);
            rig.Paint(10, 40, 30, 41, cClipped);
            const HRESULT hr = rig.Realize();
            const float max = rig.Texel(rig.maxIndex), budget = rig.Texel(rig.budgetIndex);
            const auto& stats = rig.Evaluator().DeferredCommit();
            const bool countersStill = stats.wholeDispatches == stats0.wholeDispatches &&
                                       stats.rectsDispatches == stats0.rectsDispatches &&
                                       stats.landed == stats0.landed && stats.hostedCommits == stats0.hostedCommits &&
                                       stats.thresholdCommits == stats0.thresholdCommits;
            const float host = 1234.0f;
            const HRESULT commit = rig.stats2->CommitFields(&rig.maxIndex, &host, 1);
            printf("  [info] %s: realizes 0x%08X 0x%08X; Max texel %.2f, budget texel %.6f -> %.6f; whole %d; commit 0x%08X\n",
                   name, static_cast<unsigned>(first), static_cast<unsigned>(hr), max, budget0, budget, rig.whole ? 1 : 0,
                   static_cast<unsigned>(commit));
            TEST((std::string(name) + "_RealizeOk").c_str(), first == S_OK && hr == S_OK);
            TEST((std::string(name) + "_HostedFollowsDispatch").c_str(), Near(max, 2000.0f));
            TEST((std::string(name) + "_ThresholdFollowsDispatch").c_str(),
                 std::isfinite(budget) && std::isfinite(budget0) && !SameFloat(budget, budget0));
            TEST((std::string(name) + "_DrawnWhole").c_str(), rig.whole);
            TEST((std::string(name) + "_NoDeferredCounters").c_str(), countersStill);
            TEST((std::string(name) + "_CommitFieldsRefused").c_str(), commit == E_NOT_VALID_STATE);
        };
        check("Stage3Kill_NoFlag", false);
        check("Stage3Kill_SkipOff", true);
    }

    // IMMEDIATE beside THRESHOLD on one producer: a rects dispatch copies the
    // Immediate texel at once and wakes its consumer, and leaves the
    // Threshold texel at its committed value.
    void ImmediatePolicy(bool commandList)
    {
        const std::string name = commandList ? "Stage3Immediate_CommandList" : "Stage3Immediate_Immediate";
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_IMMEDIATE;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        options.commandList = commandList;
        auto rig = MakeDeferredRig(options);
        TEST((name + "_Built").c_str(), rig.ok);
        if (!rig.ok) return;
        rig.Realize();
        rig.Drain();
        const float budget0 = rig.Committed(rig.budgetIndex);
        TEST((name + "_Seeded").c_str(), Near(rig.Committed(rig.maxIndex), 1000.0f) && rig.TexelsMatchRecord());

        const uint64_t changes0 = rig.Changes();
        rig.Paint(10, 40, 30, 41, cClipped);
        rig.Paint(5, 5, 6, 6, cPeak2000);
        rig.Realize();
        const float maxTexel = rig.Texel(rig.maxIndex), budgetTexel = rig.Texel(rig.budgetIndex);
        const bool drewWhole = rig.whole;
        rig.Drain();
        printf("  [info] %s: after the rects realize Max texel %.2f, budget texel %.6f (committed %.6f), whole %d; "
               "after landing Max %.2f, budget %.6f (landed %.6f)\n", name.c_str(), maxTexel, budgetTexel, budget0,
               drewWhole ? 1 : 0, rig.Committed(rig.maxIndex), rig.Committed(rig.budgetIndex), rig.Landed(rig.budgetIndex));
        TEST((name + "_TexelAndConsumerMoveThatRealize").c_str(), Near(maxTexel, 2000.0f) && drewWhole &&
             rig.Changes() > changes0);
        TEST((name + "_ThresholdTexelHeld").c_str(), SameFloat(budgetTexel, budget0));
        TEST((name + "_RecordMatchesAfterLanding").c_str(), Near(rig.Committed(rig.maxIndex), 2000.0f) &&
             SameFloat(rig.Committed(rig.budgetIndex), budget0) && !SameFloat(rig.Landed(rig.budgetIndex), budget0) &&
             rig.TexelsMatchRecord());
        // Nothing left: an idle realize draws nothing.
        TEST((name + "_NothingLeft").c_str(), rig.Realize() == S_OK && rig.drawn.empty());
    }

    SL_TEST_CASE(Stage3_ImmediatePolicy, Tag::Gpu)
    {
        printf("\n=== Stage 3: Immediate Max beside Threshold WhiteErrorBudget ===\n");
        ImmediatePolicy(true);
        ImmediatePolicy(false);
    }

    // The damped Hosted driver's decay: one commit after `decayPresents`
    // presents in a row below the committed value; ticks do not count.
    SL_TEST_CASE(Stage3_HostedDecay, Tag::Gpu)
    {
        printf("\n=== Stage 3: the damped Hosted driver's decay ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3Decay_Built", rig.ok);
        if (!rig.ok) return;
        constexpr uint32_t cDecay = 3;
        auto* driver = new Dda::DampedHostedDriver(rig.stats.get(), L"Max", cDecay);
        Dda::StageSim stage(rig.context.get(), rig.input.get(), rig.root.get(), rig.target.get(), g_d3dContext.get());
        stage.SetHosted(std::unique_ptr<Dda::HostedDriver>(driver));
        uint32_t ticks = 0;
        const auto settle = [&]
        {
            for (int i = 0; i < 8 && stage.TickDue(); ++i)
            {
                WaitForGpuIdle();
                stage.Tick();
                ++ticks;
            }
        };
        stage.Prime();
        settle();
        Dda::DesktopFrame frame;
        // Up: a ratchet.
        rig.Fill(5, 5, 6, 6, cPeak2000);
        frame.dirty = { RECT{ 5, 5, 6, 6 } };
        stage.Present(frame);
        settle();
        const bool ratcheted = driver->Ratchets() == 1 && Near(rig.Committed(rig.maxIndex), 2000.0f);
        // Down: the drop lands at a tick, which does not count.
        rig.Fill(5, 5, 6, 6, cPeak1000);
        stage.Present(frame);
        const uint32_t ticksBefore = ticks;
        settle();
        const bool droppedByTick = ticks > ticksBefore && Near(rig.Landed(rig.maxIndex), 1000.0f) &&
                                   Near(rig.Committed(rig.maxIndex), 2000.0f);
        // Presents with rects elsewhere: below the committed value each time.
        rig.Fill(40, 40, 41, 41, cBackground);
        frame.dirty = { RECT{ 40, 40, 41, 41 } };
        uint32_t decaysAt = 0, wholesAtDecay = 0;
        for (uint32_t p = 1; p <= cDecay + 2; ++p)
        {
            stage.Present(frame);
            if (driver->Decays() == 1 && decaysAt == 0)
            {
                decaysAt = p;
                wholesAtDecay = stage.WholeDraws();
            }
            settle();
        }
        printf("  [info] ratchet %d; drop landed by a tick %d; decay on present %u of the run below (%u decays, %u "
               "whole draw(s) then), committed %.2f\n", ratcheted ? 1 : 0, droppedByTick ? 1 : 0, decaysAt,
               driver->Decays(), wholesAtDecay, rig.Committed(rig.maxIndex));
        TEST("Stage3Decay_Arranged", ratcheted && droppedByTick);
        TEST("Stage3Decay_OneDecayAfterThePresents", decaysAt == cDecay && driver->Decays() == 1 &&
             wholesAtDecay == 1 && Near(rig.Committed(rig.maxIndex), 1000.0f) && rig.TexelsMatchRecord());
    }

    // ---- The replay: the simulated stage over the scenarios ------------------

    struct Stage3Result
    {
        bool built{ false };
        uint32_t presents{ 0 }, failed{ 0 }, rectPresents{ 0 }, ticks{ 0 };
        // Presents the OS itself reported whole (no metadata, or rects
        // covering the desktop): what a stage forwarding its rects untouched
        // forwards whole.
        uint32_t osWhole{ 0 };
        // The stage (poll first): presents and ticks it forwarded whole, by
        // cause; realizes and whole draws.
        uint32_t forwardedWhole{ 0 }, byOs{ 0 }, byHosted{ 0 }, byThreshold{ 0 }, byOwnState{ 0 }, byOther{ 0 };
        uint32_t realizes{ 0 }, wholeDraws{ 0 };
        // The drain arm.
        uint32_t drainRealizes{ 0 }, drainWholeDraws{ 0 };
        // Engine commits and their landing distance in frames; host commits.
        uint64_t commitLandings{ 0 }, landingFramesSum{ 0 }, landingFramesMax{ 0 };
        uint64_t exactCommits{ 0 }, thresholdCommits{ 0 }, hostedCommits{ 0 };
        uint32_t ratchets{ 0 }, decays{ 0 };
        // Presents and ticks on which a fallback, unskipped or fresh readback
        // ran, in either deferred arm.
        uint32_t counterFrames{ 0 };
        // Acquires whose metadata differs from what the stage drew.
        uint32_t metadataMisses{ 0 }, acquires{ 0 };
        // Acquired client surfaces compared with a forced-whole render of the
        // stage's committed values, and those that differ; realize sequences
        // (presents and ticks) in which a field committed without a whole draw.
        uint32_t compared{ 0 }, surfaceMismatches{ 0 }, commitNotWhole{ 0 };
        // Comparisons made while GetCommittedValues said a dispatch was pending.
        uint32_t pendingCompares{ 0 };
        // Instrument: the comparison finds the client off a render with
        // another peak.
        bool compareSensitive{ false };
        size_t sensitivity[4]{};
        // GPU ms from an idle GPU: each stage present, each stage tick, each
        // arm A present (dirtyRegions off, no deferred commit). CPU ms of a
        // Rects present: the stage, the stage 2 skip-on stage.
        std::vector<double> gpuMs, tickGpuMs, gpuMsA, rectCpuMs, rectCpuMsS2;
        // Instrument: presents on which the landed Max and WhiteErrorBudget
        // moved, their ranges, and budget drifts the threshold held back. A
        // replay where nothing lands moving measures nothing.
        uint32_t maxMoves{ 0 }, budgetMoves{ 0 };
        float maxLow{ INFINITY }, maxHigh{ -INFINITY }, budgetLow{ INFINITY }, budgetHigh{ -INFINITY };
        uint64_t subThreshold{ 0 };
        // Instrument: Rects presents that scheduled no deferred readback, and
        // readbacks landed. A dead pipeline commits nothing and forwards
        // nothing whole, which reads as a win.
        uint32_t rectsWithoutReadback{ 0 };
        uint64_t landings{ 0 };
        // Presents on which the true statistics moved: where they did, the
        // landed ones have to move too.
        uint32_t truthMoves{ 0 };
        // Hosted commits' distance: frames from the dispatch whose landed
        // value the host read to the frame that applied the commit.
        uint64_t hostedFramesSum{ 0 }, hostedFramesMax{ 0 };
        // The drain arm's acquires, checked as the stage's are.
        uint32_t drainAcquires{ 0 }, drainMetadataMisses{ 0 }, drainCompared{ 0 }, drainSurfaceMismatches{ 0 };
        // A wrong WhiteErrorBudget shows in the comparison too.
        bool budgetSensitive{ false };
        // Truth: arm A's statistics, read synchronously after the replay,
        // against what the stage landed and committed; and each deferred
        // arm's texture against its record.
        float truthMax{ NAN }, truthBudget{ NAN }, landedMax{ NAN }, landedBudget{ NAN }, committedBudget{ NAN };
        float budgetThreshold{ 0.0f };
        bool texelsMatch{ false };
        // The stage's cell reductions over the replay (incrementalReductions on).
        Rendering::ReduceCellStats cells;
        // Stage 5 (moves handed to the engine): the stage's moves copied and
        // drawn at their destinations; compared pixels within the dither of
        // a dithering tone map's moved output (not counted as mismatches).
        uint64_t movesApplied{ 0 }, movesCollapsed{ 0 };
        size_t surfaceDrift{ 0 };
    };

    // The moves, in order, onto 4-byte pixels (rows packed), each through a copy.
    void MoveBytes(std::vector<uint8_t>& image, uint32_t width, const std::vector<EFFECTGRAPH_MOVE_RECT>& moves)
    {
        for (const auto& move : moves)
        {
            const RECT& d = move.DestinationRect;
            const LONG w = d.right - d.left, h = d.bottom - d.top;
            if (w <= 0 || h <= 0) continue;
            std::vector<uint8_t> block(static_cast<size_t>(w) * h * 4);
            for (LONG y = 0; y < h; ++y)
                std::memcpy(&block[static_cast<size_t>(y) * w * 4],
                            &image[(static_cast<size_t>(move.SourcePoint.y + y) * width + move.SourcePoint.x) * 4],
                            static_cast<size_t>(w) * 4);
            for (LONG y = 0; y < h; ++y)
                std::memcpy(&image[(static_cast<size_t>(d.top + y) * width + d.left) * 4],
                            &block[static_cast<size_t>(y) * w * 4], static_cast<size_t>(w) * 4);
        }
    }

    // The desktop duplication client contract, on the CPU and independent of
    // how the stage built its metadata: a client keeps its last image, applies
    // a delivery's moves to it, then copies the delivery's dirty rects from the
    // surface. After every acquire that must give the stage's target exactly.
    struct ClientModel
    {
        std::vector<uint8_t> image;
        uint32_t width{ 0 }, height{ 0 };

        void Start(std::vector<uint8_t> target, uint32_t w, uint32_t h)
        {
            image = std::move(target);
            width = w;
            height = h;
        }
        // True when the model, after `delivery`, equals `target`.
        bool Apply(const Dda::Delivery& delivery, const std::vector<uint8_t>& target)
        {
            if (delivery.whole || image.size() != target.size())
            {
                image = target;
                return delivery.whole;
            }
            MoveBytes(image, width, delivery.moves);
            for (const auto& r : delivery.dirty)
            {
                const RECT c = ClipRect(r, width, height);
                for (LONG y = c.top; y < c.bottom; ++y)
                    std::memcpy(&image[(static_cast<size_t>(y) * width + c.left) * 4],
                                &target[(static_cast<size_t>(y) * width + c.left) * 4],
                                static_cast<size_t>(c.right - c.left) * 4);
            }
            return image == target;
        }
    };

    // Pixels of two 4-byte images that differ outside `drift` (a mask of
    // moved pixels, one byte each), or by more than `codes` in a channel
    // inside it; `within` gets those inside it differing by `codes` or less.
    size_t BeyondDrift(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int codes,
                       const std::vector<uint8_t>& drift, size_t& within)
    {
        size_t beyond = 0;
        const size_t pixels = (std::min)(a.size(), b.size()) / 4;
        for (size_t i = 0; i < pixels; ++i)
        {
            int distance = 0;
            for (int c = 0; c < 4; ++c) distance = (std::max)(distance, std::abs(static_cast<int>(a[i * 4 + c]) - b[i * 4 + c]));
            if (!distance) continue;
            const bool moved = i < drift.size() && drift[i];
            if (!moved || distance > codes) ++beyond;
            else ++within;
        }
        return beyond;
    }

    // `moves`: the stage and the drain arm hand the engine each frame's moves
    // (stage 5); a client surface may then differ from the whole render by
    // cMoveDitherCodes where pixels were moved since a realize last drew
    // them (StageSim::Drift), and must equal it everywhere else.
    Stage3Result Stage3Replay(uint32_t width, uint32_t height, const FrameSource& source, bool moves = false)
    {
        Stage3Result result;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        if (!source.start(desktop)) return result;

        RigOptions deferred;
        deferred.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        deferred.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        deferred.shared = &desktop;
        RigOptions armA = deferred;
        armA.deferred = false;
        armA.dirtyRegions = false;
        RigOptions stage2 = deferred;
        stage2.deferred = false;
        // The reference: dirtyRegions and caching off, the tone map's two
        // statistics-bound properties unbound and set to the stage's committed
        // values before each whole render.
        RigOptions reference = armA;
        reference.bindStatistics = false;
        DeferredRig rigs[5] = { MakeDeferredRig(deferred), MakeDeferredRig(deferred), MakeDeferredRig(armA),
                                MakeDeferredRig(stage2), MakeDeferredRig(reference) };
        for (const auto& rig : rigs)
            if (!rig.ok) return result;
        auto& stageRig = rigs[0];
        auto& drainRig = rigs[1];
        auto& referenceRig = rigs[4];
        std::unique_ptr<Dda::StageSim> stages[4];
        for (int a = 0; a < 4; ++a)
            stages[a] = std::make_unique<Dda::StageSim>(rigs[a].context.get(), rigs[a].input.get(), rigs[a].root.get(),
                                                        rigs[a].target.get(), g_d3dContext.get());
        auto* hosted = new Dda::DampedHostedDriver(stageRig.stats.get(), L"Max");
        stages[0]->SetHosted(std::unique_ptr<Dda::HostedDriver>(hosted));
        auto* drainHosted = new Dda::DampedHostedDriver(drainRig.stats.get(), L"Max");
        stages[1]->SetHosted(std::unique_ptr<Dda::HostedDriver>(drainHosted));
        stages[1]->SetPollFirst(false);
        stages[0]->SetMoves(moves);
        stages[1]->SetMoves(moves);
        stages[0]->SetTrackDrift(moves);
        stages[1]->SetTrackDrift(moves);
        const auto movesStart = stageRig.Evaluator().RegionStats();
        auto client = MakeClientSurface(width, height);
        const uint32_t clientIndex = stages[0]->AddClient(client.get());
        auto drainClient = MakeClientSurface(width, height);
        const uint32_t drainClientIndex = stages[1]->AddClient(drainClient.get());

        auto& stage = *stages[0];
        auto& drain = *stages[1];
        const auto counts = [&] { return std::pair{ CountsOf(stageRig.Evaluator().ReadbackStats()),
                                                    CountsOf(drainRig.Evaluator().ReadbackStats()) }; };
        // Settles a deferred arm: ticks until none is due.
        const auto settle = [&](Dda::StageSim& sim, uint32_t& ticks)
        {
            for (int i = 0; i < 8 && sim.TickDue(); ++i)
            {
                WaitForGpuIdle();
                if (FAILED(sim.Tick())) return false;
                ++ticks;
            }
            return true;
        };
        for (auto& sim : stages)
            if (FAILED(sim->Prime())) return result;
        uint32_t settleTicks = 0;
        if (!settle(stage, settleTicks) || !settle(drain, settleTicks)) return result;
        stage.Acquire(clientIndex);
        drain.Acquire(drainClientIndex);
        ClientModel stageModel, drainModel;
        stageModel.Start(Dda::ReadBytes(stageRig.target.get()), width, height);
        drainModel.Start(Dda::ReadBytes(drainRig.target.get()), width, height);
        result.built = true;
        const auto cellsStart = stageRig.Evaluator().ReduceCells();

        // The replay's own counters: a running maximum starts here.
        stageRig.Evaluator().ResetDeferredCommitStats();
        const auto commitStats = stageRig.Evaluator().DeferredCommit();
        GpuStopwatch stopwatch;
        auto referenceProps = referenceRig.root.as<ID2D1Properties>();
        const auto setReference = [&](const wchar_t* property, float value)
        {
            return SUCCEEDED(referenceProps->SetValueByName(property, D2D1_PROPERTY_TYPE_FLOAT,
                                                            reinterpret_cast<const BYTE*>(&value), sizeof(value)));
        };
        // A client's surface against the whole target rendered with the
        // values its arm committed: true when they differ.
        const auto surfaceDiffers = [&](DeferredRig& rig, ID3D11Texture2D* surface, const std::vector<uint8_t>& drift)
        {
            // While a dispatch's values are drawn but not read back, the
            // record says so (S_FALSE), and the texels hold what was drawn.
            const bool pending = rig.CommitPending();
            if (pending) ++result.pendingCompares;
            const float max = pending ? rig.Texel(rig.maxIndex) : rig.Committed(rig.maxIndex);
            const float budget = pending ? rig.Texel(rig.budgetIndex) : rig.Committed(rig.budgetIndex);
            referenceRig.input2->Invalidate();
            if (!std::isfinite(max) || !std::isfinite(budget) || !setReference(L"SourcePeakNits", max) ||
                !setReference(L"WhiteErrorBudget", budget) || FAILED(referenceRig.Realize()))
                return true;
            const auto surfaceBytes = Dda::ReadBytes(surface);
            const auto referenceBytes = Dda::ReadBytes(referenceRig.target.get());
            const bool differs = !moves
                ? Dda::Compare(surfaceBytes, referenceBytes, width, {}).differing != 0
                : BeyondDrift(surfaceBytes, referenceBytes, Rendering::cMoveDitherCodes, drift, result.surfaceDrift) != 0;
            if (differs)
            {
                // Where and by how much, so a mismatch on a recording can be traced to its present.
                size_t count = 0;
                int worst = 0;
                LONG l = LONG_MAX, t = LONG_MAX, r = -1, b = -1;
                const size_t pixels = (std::min)(surfaceBytes.size(), referenceBytes.size()) / 4;
                for (size_t i = 0; i < pixels; ++i)
                {
                    int d = 0;
                    for (int c = 0; c < 4; ++c)
                        d = (std::max)(d, std::abs(static_cast<int>(surfaceBytes[i * 4 + c]) - referenceBytes[i * 4 + c]));
                    if (!d) continue;
                    ++count;
                    worst = (std::max)(worst, d);
                    const LONG x = static_cast<LONG>(i % width), y = static_cast<LONG>(i / width);
                    l = (std::min)(l, x); t = (std::min)(t, y); r = (std::max)(r, x + 1); b = (std::max)(b, y + 1);
                }
                printf("  [info] surface mismatch (%s arm) after present %u: %zu px differ by up to %d codes in "
                       "(%ld,%ld)-(%ld,%ld); committed Max %.4f, budget %.6f; landed Max %.4f, budget %.6f\n",
                       &rig == &stageRig ? "stage" : "drain", result.presents, count, worst, l, t, r, b, max, budget,
                       rig.Landed(rig.maxIndex), rig.Landed(rig.budgetIndex));
            }
            return differs;
        };
        const auto compareSurface = [&]
        {
            ++result.compared;
            if (surfaceDiffers(stageRig, client.get(), stage.Drift())) ++result.surfaceMismatches;
        };
        // The drain arm after a present or tick: its metadata and surface.
        const auto accountDrain = [&]
        {
            result.drainRealizes += drain.Realizes();
            result.drainWholeDraws += drain.WholeDraws();
            const auto delivery = drain.Acquire(drainClientIndex);
            ++result.drainAcquires;
            const bool drewWhole = drain.WholeDraws() > 0;
            // The metadata against a client that applies it to its own copy.
            if (delivery.hr != S_OK || delivery.whole != drewWhole ||
                !drainModel.Apply(delivery, Dda::ReadBytes(drainRig.target.get())))
                ++result.drainMetadataMisses;
            if (delivery.hr != S_OK) return;
            ++result.drainCompared;
            if (surfaceDiffers(drainRig, drainClient.get(), drain.Drift())) ++result.drainSurfaceMismatches;
        };
        // A stage realize sequence (present or tick): whole forwards by cause,
        // and the client's metadata against what was drawn.
        const auto account = [&](bool covering, const Rendering::DeferredCommitStats& before)
        {
            const auto& now = stageRig.Evaluator().DeferredCommit();
            result.realizes += stage.Realizes();
            result.wholeDraws += stage.WholeDraws();
            // A commit is drawn whole by the sequence that made it.
            if ((now.commitLandings > before.commitLandings || now.hostedCommits > before.hostedCommits) &&
                stage.WholeDraws() == 0)
                ++result.commitNotWhole;
            if (stage.WholeDraws() > 0)
            {
                ++result.forwardedWhole;
                if (covering) ++result.byOs;
                else if (stage.HostedCommitted()) ++result.byHosted;
                else if (now.thresholdCommits + now.exactCommits > before.thresholdCommits + before.exactCommits)
                    ++result.byThreshold;
                else if (now.wholeDispatches > before.wholeDispatches) ++result.byOwnState;
                else ++result.byOther;
            }
            const auto delivery = stage.Acquire(clientIndex);
            ++result.acquires;
            const bool drewWhole = stage.WholeDraws() > 0;
            if (delivery.hr != S_OK || delivery.whole != drewWhole ||
                !stageModel.Apply(delivery, Dda::ReadBytes(stageRig.target.get())))
                ++result.metadataMisses;
            if (delivery.hr == S_OK) compareSurface();
        };
        // A stage tick, timed from an idle GPU.
        const auto tick = [&]
        {
            const auto before = stageRig.Evaluator().DeferredCommit();
            WaitForGpuIdle();
            stopwatch.Begin();
            if (FAILED(stage.Tick())) ++result.failed;
            result.tickGpuMs.push_back(stopwatch.End());
            ++result.ticks;
            account(false, before);
        };

        // Truth: the reference arm (dirtyRegions off, nothing bound to its
        // statistics) computes them whole from the same desktop and reads
        // them back synchronously. Untimed, and off the arms being measured.
        const auto truthNow = [&](float& max, float& budget)
        {
            max = budget = NAN;
            referenceRig.input2->Invalidate();
            winrt::com_ptr<IEffectGraphValues> truth;
            if (FAILED(referenceRig.stats->GetValues(truth.put())) || !truth) return;
            truth->GetFloat(L"Max", &max);
            truth->GetFloat(L"WhiteErrorBudget", &budget);
        };
        float lastTruthMax = NAN, lastTruthBudget = NAN;
        truthNow(lastTruthMax, lastTruthBudget);

        float lastMax = stageRig.Landed(stageRig.maxIndex), lastBudget = stageRig.Landed(stageRig.budgetIndex);
        Dda::DesktopFrame frame;
        while (source.next(desktop, frame))
        {
            const auto countsBefore = counts();
            if (frame.kind == Dda::FrameKind::NoFrame)
            {
                // An idle moment: the retry tick, where one is due.
                if (stage.TickDue()) tick();
                if (drain.TickDue())
                {
                    WaitForGpuIdle();
                    if (SUCCEEDED(drain.Tick())) accountDrain();
                    else ++result.failed;
                }
                if (counts() != countsBefore) ++result.counterFrames;
                continue;
            }
            ++result.presents;
            const auto changed = Dda::ChangedRects(frame);
            const bool covering = frame.whole || CoversSurface(changed, width, height);
            if (covering) ++result.osWhole;
            else ++result.rectPresents;

            // The drain arm, untimed and first: the first arm to read the
            // frame's desktop upload pays for it (measured on the video
            // scenarios: whichever timed arm came first looked 0.1-0.4 ms
            // dearer on the CPU), so no timed arm does.
            WaitForGpuIdle();
            if (FAILED(drain.Present(frame))) ++result.failed;
            else accountDrain();

            // The stage and the stage 2 skip-on stage, each timed from an
            // idle GPU, in alternating order: what ran just before still
            // moved the CPU time of the next by tenths of a millisecond.
            const auto timeStage = [&]
            {
                const auto before = stageRig.Evaluator().DeferredCommit();
                const uint64_t readbacksBefore = stageRig.Evaluator().ReadbackStats().deferredReadbacks;
                WaitForGpuIdle();
                const auto start = std::chrono::steady_clock::now();
                stopwatch.Begin();
                const HRESULT hr = stage.Present(frame);
                const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                result.gpuMs.push_back(stopwatch.End());
                if (!covering) result.rectCpuMs.push_back(cpuMs);
                if (FAILED(hr)) ++result.failed;
                if (!covering && stageRig.Evaluator().ReadbackStats().deferredReadbacks == readbacksBefore)
                    ++result.rectsWithoutReadback;
                account(covering, before);
            };
            const auto timeStage2 = [&]
            {
                WaitForGpuIdle();
                const auto start = std::chrono::steady_clock::now();
                if (FAILED(stages[3]->Present(frame))) ++result.failed;
                const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (!covering) result.rectCpuMsS2.push_back(cpuMs);
            };
            if (result.presents % 2 == 0)
            {
                timeStage();
                timeStage2();
            }
            else
            {
                timeStage2();
                timeStage();
            }
            const float landedMax = stageRig.Landed(stageRig.maxIndex);
            const float landedBudget = stageRig.Landed(stageRig.budgetIndex);
            if (!SameFloat(landedMax, lastMax)) ++result.maxMoves;
            if (!SameFloat(landedBudget, lastBudget)) ++result.budgetMoves;
            lastMax = landedMax;
            lastBudget = landedBudget;
            result.maxLow = (std::min)(result.maxLow, landedMax);
            result.maxHigh = (std::max)(result.maxHigh, landedMax);
            result.budgetLow = (std::min)(result.budgetLow, landedBudget);
            result.budgetHigh = (std::max)(result.budgetHigh, landedBudget);
            float truthMax = NAN, truthBudget = NAN;
            truthNow(truthMax, truthBudget);
            if (!SameFloat(truthMax, lastTruthMax) || !SameFloat(truthBudget, lastTruthBudget)) ++result.truthMoves;
            lastTruthMax = truthMax;
            lastTruthBudget = truthBudget;

            // Arm A: dirtyRegions off.
            WaitForGpuIdle();
            stopwatch.Begin();
            if (FAILED(stages[2]->Present(frame))) ++result.failed;
            result.gpuMsA.push_back(stopwatch.End());
            if (counts() != countsBefore) ++result.counterFrames;
        }
        // The desktop goes idle: the ticks land what is still pending.
        for (int i = 0; i < 8 && stage.TickDue(); ++i)
        {
            const auto countsBefore = counts();
            tick();
            if (counts() != countsBefore) ++result.counterFrames;
        }
        for (int i = 0; i < 8 && drain.TickDue(); ++i)
        {
            const auto countsBefore = counts();
            WaitForGpuIdle();
            if (SUCCEEDED(drain.Tick())) accountDrain();
            else ++result.failed;
            if (counts() != countsBefore) ++result.counterFrames;
        }

        {
            const float max = stageRig.Committed(stageRig.maxIndex);
            const float budget = stageRig.Committed(stageRig.budgetIndex);
            const auto differing = [&](float peak, float budgetValue) -> size_t
            {
                referenceRig.input2->Invalidate();
                if (!setReference(L"SourcePeakNits", peak) || !setReference(L"WhiteErrorBudget", budgetValue) ||
                    FAILED(referenceRig.Realize()))
                    return 0;
                return Dda::Compare(Dda::ReadBytes(client.get()), Dda::ReadBytes(referenceRig.target.get()), width, {}).differing;
            };
            // A higher peak (the stand-in's exposure follows it) and an 81-nit
            // one (the plugin tone map ignores a peak above its content, but
            // not one below SDR white).
            result.sensitivity[0] = differing(2.0f * max + 100.0f, budget);
            result.sensitivity[1] = differing(81.0f, budget);
            result.compareSensitive = result.sensitivity[0] > 0 || result.sensitivity[1] > 0;
            // A wrong budget: half of it (as stage 0 checks), and one moved up
            // by more than any threshold.
            result.sensitivity[2] = differing(max, budget * 0.5f);
            result.sensitivity[3] = differing(max, budget * 2.0f + 0.25f);
            result.budgetSensitive = result.sensitivity[2] > 0 || result.sensitivity[3] > 0;
        }
        // Truth for the last desktop.
        {
            truthNow(result.truthMax, result.truthBudget);
            result.landedMax = stageRig.Landed(stageRig.maxIndex);
            result.landedBudget = stageRig.Landed(stageRig.budgetIndex);
            result.committedBudget = stageRig.Committed(stageRig.budgetIndex);
            const auto* statsNode = stageRig.Model().FindNode(stageRig.statsId);
            result.budgetThreshold = statsNode ? statsNode->customEffect->analysisFields[stageRig.budgetIndex].changeThreshold
                                               : 0.0f;
            result.texelsMatch = stageRig.TexelsMatchRecord() && drainRig.TexelsMatchRecord();
        }
        const auto& after = stageRig.Evaluator().DeferredCommit();
        result.landings = after.landed - commitStats.landed;
        result.hostedFramesSum = after.hostedFramesSum - commitStats.hostedFramesSum;
        result.hostedFramesMax = after.hostedFramesMax;
        result.commitLandings = after.commitLandings - commitStats.commitLandings;
        result.landingFramesSum = after.landingFramesSum - commitStats.landingFramesSum;
        // Reset at the replay's start: the maximum is this replay's.
        result.landingFramesMax = after.landingFramesMax;
        result.exactCommits = after.exactCommits - commitStats.exactCommits;
        result.thresholdCommits = after.thresholdCommits - commitStats.thresholdCommits;
        result.hostedCommits = after.hostedCommits - commitStats.hostedCommits;
        result.ratchets = hosted->Ratchets();
        result.decays = hosted->Decays();
        result.subThreshold = after.subThreshold - commitStats.subThreshold;
        const auto& cells = stageRig.Evaluator().ReduceCells();
        result.cells = { cells.dispatches - cellsStart.dispatches, cells.incremental - cellsStart.incremental,
                         cells.cellsDispatched - cellsStart.cellsDispatched, cells.cellsTotal - cellsStart.cellsTotal };
        const auto& movesEnd = stageRig.Evaluator().RegionStats();
        result.movesApplied = movesEnd.movesApplied - movesStart.movesApplied;
        result.movesCollapsed = movesEnd.movesCollapsed - movesStart.movesCollapsed;
        return result;
    }

    void ReportStage3(const std::string& name, const Stage3Result& r)
    {
        const auto per100 = [&](double n) { return r.presents ? 100.0 * n / r.presents : 0.0; };
        // The instrument first: every Rects present scheduled a readback,
        // readbacks landed, and the landed values moved. Without that, a
        // replay forwarding nothing whole measured a dead pipeline.
        const bool readbacksLive = r.rectsWithoutReadback == 0 && r.landings > 0;
        const bool valuesMoved = r.maxMoves + r.budgetMoves > 0;
        const bool live = readbacksLive && (r.truthMoves == 0 || valuesMoved);
        // The stage's landed values against arm A's, computed whole and read
        // synchronously: Max exactly; the budget, a GPU sum, to 1e-4 relative;
        // the committed budget within its threshold of them.
        const bool landedTrue = SameFloat(r.landedMax, r.truthMax) && std::isfinite(r.truthBudget) &&
                                std::fabs(r.landedBudget - r.truthBudget) <= 1e-4f * (std::max)(1.0f, std::fabs(r.truthBudget));
        const bool committedTrue = std::isfinite(r.committedBudget) &&
                                   std::fabs(r.committedBudget - r.truthBudget) <= r.budgetThreshold + 1e-4f;
        printf("  [info] %s: instrument: %u Rects present(s) without a deferred readback, %llu readback(s) landed; the "
               "true statistics moved on %u present(s), the landed ones moved %d -> %s\n", name.c_str(),
               r.rectsWithoutReadback, static_cast<unsigned long long>(r.landings), r.truthMoves, valuesMoved ? 1 : 0,
               live ? "live" : "NOT LIVE: every number below is VOID");
        printf("  [info] %s: truth (reference arm, synchronous): Max %.4f, budget %.6f; landed Max %.4f, budget %.6f; committed "
               "budget %.6f (threshold %.3f); texture equals record %d\n", name.c_str(), r.truthMax, r.truthBudget,
               r.landedMax, r.landedBudget, r.committedBudget, r.budgetThreshold, r.texelsMatch ? 1 : 0);
        printf("  [info] %s: %u presents (%u with rects), %u ticks, %u failed\n", name.c_str(), r.presents,
               r.rectPresents, r.ticks, r.failed);
        printf("  [info] %s: forwarded whole %.1f per 100 presents (OS's own %.1f): OS %u, Hosted commits %u, "
               "Threshold/Exact commits %u, own-state whole dispatches %u, other %u\n", name.c_str(),
               per100(r.forwardedWhole), per100(r.osWhole), r.byOs, r.byHosted, r.byThreshold, r.byOwnState, r.byOther);
        printf("  [info] %s: whole draws per present %.3f poll first (%u realizes), %.3f on the drain arm (%u realizes)\n",
               name.c_str(), r.presents ? static_cast<double>(r.wholeDraws) / r.presents : 0.0, r.realizes,
               r.presents ? static_cast<double>(r.drainWholeDraws) / r.presents : 0.0, r.drainRealizes);
        printf("  [info] %s: engine commits %llu (Exact fields %llu, Threshold fields %llu), landing distance mean %.2f, "
               "max %llu frames; Hosted commits %llu (%u ratchets, %u decays)\n", name.c_str(),
               static_cast<unsigned long long>(r.commitLandings), static_cast<unsigned long long>(r.exactCommits),
               static_cast<unsigned long long>(r.thresholdCommits),
               r.commitLandings ? static_cast<double>(r.landingFramesSum) / r.commitLandings : 0.0,
               static_cast<unsigned long long>(r.landingFramesMax), static_cast<unsigned long long>(r.hostedCommits),
               r.ratchets, r.decays);
        printf("  [info] %s: Hosted commit distance (landed dispatch to applied commit) mean %.2f, max %llu frames\n",
               name.c_str(), r.hostedCommits ? static_cast<double>(r.hostedFramesSum) / r.hostedCommits : 0.0,
               static_cast<unsigned long long>(r.hostedFramesMax));
        printf("  [info] %s: instrument: landed Max moved on %u presents (%.2f to %.2f nits), WhiteErrorBudget on %u "
               "(%.6f to %.6f), %llu drift(s) held back by its threshold\n", name.c_str(), r.maxMoves, r.maxLow,
               r.maxHigh, r.budgetMoves, r.budgetLow, r.budgetHigh, static_cast<unsigned long long>(r.subThreshold));
        // GPU cost over the whole replay: every present and every tick the
        // stage made, against arm A's presents (it has no ticks). A median
        // alone hides whole draws on fewer than half the presents.
        const auto sum = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s; };
        const double gpuTotal = sum(r.gpuMs) + sum(r.tickGpuMs), gpuTotalA = sum(r.gpuMsA);
        const double gpuMean = r.presents ? gpuTotal / r.presents : NAN;
        const double gpuMeanA = r.gpuMsA.empty() ? NAN : gpuTotalA / r.gpuMsA.size();
        const bool gpuMeasured = !std::isnan(gpuMean) && !std::isnan(gpuMeanA);
        printf("  [info] %s: GPU ms per present, ticks included: mean %.3f (total %.3f over %u presents and %u ticks; "
               "present median %.3f) vs arm A (dirtyRegions off) mean %.3f (median %.3f) -> %s\n", name.c_str(),
               gpuMean, gpuTotal, r.presents, r.ticks, Median(r.gpuMs), gpuMeanA, Median(r.gpuMsA),
               !gpuMeasured ? "not measured" : (gpuTotal < gpuTotalA ? "below" : "NOT below"));
        const double rectCpu = Median(r.rectCpuMs), rectCpuS2 = Median(r.rectCpuMsS2);
        printf("  [info] %s: Rects-present CPU ms median %.3f (mean %.3f) vs stage 2 skip-on %.3f (mean %.3f) over %zu "
               "(%s)\n", name.c_str(), rectCpu, Mean(r.rectCpuMs), rectCpuS2, Mean(r.rectCpuMsS2), r.rectCpuMs.size(),
               g_useWarp ? "WARP: not a hardware number" : "hardware");
        printf("  [info] %s: exit gate: forwarded-whole excess over the OS rate %+.1f per 100 presents; margin unset "
               "(design section 8.1), so the gate is pending; damping provisional (section 8)\n", name.c_str(),
               per100(r.forwardedWhole) - per100(r.osWhole));
        if (r.presents < 30)
            printf("  [info] %s: the damped driver decays after 30 presents below; this replay has %u, so its Hosted "
                   "commits are ratchets only (Stage3_HostedDecay covers the decay)\n", name.c_str(), r.presents);
        printf("  [info] %s: %u of %u acquired surfaces differ from a whole render of the committed values (%u compared "
               "while a dispatch was pending, against its texels); %u realize sequence(s) committed without a whole "
               "draw; instrument: a wrong peak shows on %zu pixels (2 x Max + 100) and %zu (81 nits), a wrong budget on "
               "%zu (half) and %zu (2 x + 0.25)\n", name.c_str(), r.surfaceMismatches, r.compared, r.pendingCompares,
               r.commitNotWhole, r.sensitivity[0], r.sensitivity[1], r.sensitivity[2], r.sensitivity[3]);
        printf("  [info] %s: drain arm: %u of %u acquired surfaces differ from a whole render of its committed values; "
               "%u of %u acquires' metadata differ from what it drew\n", name.c_str(), r.drainSurfaceMismatches,
               r.drainCompared, r.drainMetadataMisses, r.drainAcquires);
        printf("  [info] %s: cell reductions: %llu dispatches, %llu over fewer than 64 cells; %llu of %llu cells "
               "recomputed\n", name.c_str(), static_cast<unsigned long long>(r.cells.dispatches),
               static_cast<unsigned long long>(r.cells.incremental), static_cast<unsigned long long>(r.cells.cellsDispatched),
               static_cast<unsigned long long>(r.cells.cellsTotal));
        TEST((name + "_Built").c_str(), r.built && r.presents > 0 && r.failed == 0);
        TEST((name + "_LivePipeline").c_str(), live);
        TEST((name + "_LandedEqualsTruth").c_str(), landedTrue);
        TEST((name + "_CommittedWithinThresholdOfTruth").c_str(), committedTrue);
        TEST((name + "_TexturesEqualRecords").c_str(), r.texelsMatch);
        // A wrong budget shows in the surface comparison (the stand-in), or,
        // where the tone map leaves those pixels alone (the plugin on most
        // scenarios), the budget is held to the truth above instead.
        printf("  [info] %s: WhiteErrorBudget validated by %s\n", name.c_str(),
               r.budgetSensitive ? "the surface comparison and the truth" : "the truth only (a wrong budget shows on no pixel)");
        TEST((name + "_BudgetValidated").c_str(), r.budgetSensitive || (landedTrue && committedTrue));
        TEST((name + "_DrainArmMetadataMatchesDrawn").c_str(), r.drainMetadataMisses == 0 && r.drainAcquires > 0);
        TEST((name + "_DrainArmSurfaceEqualsCommittedWhole").c_str(), r.drainSurfaceMismatches == 0 &&
             r.drainCompared == r.drainAcquires);
        TEST((name + "_NoFallbackUnskippedOrFreshAnyFrame").c_str(), r.counterFrames == 0);
        TEST((name + "_ForwardedMetadataMatchesDrawn").c_str(), r.metadataMisses == 0 && r.acquires > 0);
        TEST((name + "_SurfaceEqualsCommittedWhole").c_str(), r.surfaceMismatches == 0 && r.compared == r.acquires);
        TEST((name + "_SurfaceCompareSensitive").c_str(), r.compareSensitive);
        TEST((name + "_EveryCommitDrawnWhole").c_str(), r.commitNotWhole == 0);
        // Rects presents cost what stage 2's skip-on presents cost, within a
        // quarter and 0.1 ms (the deferred commit adds one non-blocking landing).
        if (!r.rectCpuMs.empty() && !r.rectCpuMsS2.empty())
            TIMING_TEST((name + "_RectsCpuAtStage2Figure").c_str(), rectCpu <= rectCpuS2 * 1.25 + 0.1);
        // Every whole forward has a cause the harness can name.
        TEST((name + "_EveryWholeForwardExplained").c_str(), r.byOther == 0);
        // The drain arm never forwards fewer whole draws than the stage.
        TEST((name + "_DrainArmCostsAtLeastPollFirst").c_str(), r.drainWholeDraws + 1 >= r.wholeDraws);
    }

    SL_TEST_CASE(Stage3_ReplaySynthetic, Tag::Gpu)
    {
        printf("\n=== Stage 3: the simulated stage, deferred commit, synthetic scenarios (%ls) ===\n", GateToneMapName());
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        std::vector<Dda::ScenarioKind> kinds(std::begin(cScenarios), std::end(cScenarios));
        kinds.push_back(Dda::ScenarioKind::HdrHighlights);
        uint64_t hostedCommits = 0, thresholdCommits = 0, landings = 0, incrementalCells = 0;
        for (const auto kind : kinds)
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = Stage3Replay(width, height, SyntheticSource(*scenario));
            ReportStage3(std::string("Stage3_") + Dda::ScenarioName(kind), result);
            incrementalCells += result.cells.incremental;
            hostedCommits += result.hostedCommits;
            thresholdCommits += result.thresholdCommits;
            landings += result.commitLandings;
        }
        // Positive control: the replay committed both policies' fields, so
        // its counters and metadata checks saw commits.
        TEST("Stage3_ReplayCommittedHostedAndThreshold", hostedCommits > 0 && thresholdCommits > 0 && landings > 0);
        // Stage 4: Rects presents reach the statistics as rects, so dispatches
        // recompute fewer than all 64 cells (not on every scenario: past
        // DirtyRegion::cMaxRects the rects collapse to a box that can touch
        // every cell, as VideoAndScroll's do).
        TEST("Stage3_ReplayRecomputedFewerCells", incrementalCells > 0);
    }

    // The plugin repository's DDA graph declares the policies: Hosted on
    // SourcePeakNits, Threshold on WhiteErrorBudget (0.05 dE ITP, provisional).
    // Its first realize under NO_FALLBACK seeds the record with no fallback.
    SL_TEST_CASE(Stage3_DdaGraphPolicies, Tag::Gpu)
    {
        printf("\n=== Stage 3: the DDA graph's commit policies ===\n");
        if (g_pluginPath.empty())
        {
            ReportNotRun("Stage3_DdaGraphPolicies: needs --plugin <PrivateEffects.dll>");
            return;
        }
        const auto graphs = FindPluginGraphs(L"dda_screenshot.json");
        if (graphs.empty())
        {
            ReportNotRun("Stage3_DdaGraphPolicies: no graphs/dda_screenshot.json beside the plugin");
            return;
        }
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), 64, 64);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                desktop.Put(x, y, cBackground, cBackground, cBackground);
        desktop.Put(5, 5, cPeak1000, cPeak1000, cPeak1000);
        desktop.UploadAll();
        auto context = CreateDeferredContext(EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK);
        const auto g = LoadDdaGraph(*context, graphs / L"dda_screenshot.json", desktop.Texture(), true, "Stage3Dda");
        if (!g.built) return;

        const auto& model = context->Model();
        const auto* tonemap = model.FindNode(g.tonemapId);
        const auto* stats = model.FindNode(g.statsId);
        const auto policyOf = [&](const wchar_t* property)
        {
            const auto it = tonemap->propertyBindings.find(property);
            return it == tonemap->propertyBindings.end() ? Graph::CommitPolicy::Exact : it->second.commitPolicy;
        };
        const UINT32 budgetIndex = FieldIndex(stats, L"WhiteErrorBudget");
        const UINT32 maxIndex = FieldIndex(stats, L"Max");
        const float threshold = budgetIndex != EFFECTGRAPH_INVALID_INDEX
            ? stats->customEffect->analysisFields[budgetIndex].changeThreshold : 0.0f;
        printf("  [info] SourcePeakNits %ls, WhiteErrorBudget %ls (changeThreshold %.3f)\n",
               Graph::CommitPolicyTag(policyOf(L"SourcePeakNits")), Graph::CommitPolicyTag(policyOf(L"WhiteErrorBudget")),
               threshold);
        TEST("Stage3Dda_HostedSourcePeakNits", policyOf(L"SourcePeakNits") == Graph::CommitPolicy::Hosted);
        TEST("Stage3Dda_ThresholdWhiteErrorBudget", policyOf(L"WhiteErrorBudget") == Graph::CommitPolicy::Threshold &&
             threshold == 0.05f);

        auto target = MakeSrgbTarget(64, 64);
        auto context2 = context.as<IEffectGraphContext2>();
        const uint64_t fallback0 = context->Evaluator().ReadbackStats().fallbackReadbacks;
        RECT drawn[4]{};
        UINT32 count = 0;
        HRESULT hr = context2->RealizeToIncremental(g.root.get(), target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL,
                                                    drawn, 4, &count, nullptr, 0, nullptr);
        const HRESULT first = hr;
        for (int i = 0; i < 16 && hr == S_FALSE; ++i)
        {
            WaitForGpuIdle();
            hr = context2->RealizeToIncremental(g.root.get(), target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL,
                                                drawn, 4, &count, nullptr, 0, nullptr);
        }
        winrt::com_ptr<IEffectGraphValues> committed;
        auto stats2 = g.stats.as<IEffectGraphNode2>();
        const bool seeded = SUCCEEDED(stats2->GetCommittedValues(committed.put())) && committed;
        std::vector<float> texels;
        context->Evaluator().ReadAnalysisTexture(g.statsId, texels);
        float committedMax = NAN;
        if (seeded) committed->GetFloat(L"Max", &committedMax);
        size_t maxTexel = 0;
        for (UINT32 i = 0; i < maxIndex && i < stats->customEffect->analysisFields.size(); ++i)
            maxTexel += stats->customEffect->analysisFields[i].pixelCount();
        const float texelMax = maxTexel * 4 < texels.size() ? texels[maxTexel * 4] : NAN;
        printf("  [info] NO_FALLBACK: first realize 0x%08X, settled 0x%08X; committed Max %.2f, texel %.2f; %llu "
               "fallback readback(s)\n", static_cast<unsigned>(first), static_cast<unsigned>(hr), committedMax, texelMax,
               static_cast<unsigned long long>(context->Evaluator().ReadbackStats().fallbackReadbacks - fallback0));
        TEST("Stage3Dda_NoFallbackFirstRealize", SUCCEEDED(first) && hr == S_OK &&
             context->Evaluator().ReadbackStats().fallbackReadbacks == fallback0);
        TEST("Stage3Dda_HostedSeeded", seeded && Near(committedMax, 1000.0f) && SameFloat(texelMax, committedMax));
    }

    // The stage on a desktop that goes idle: the retry tick lands what the
    // last present scheduled, commits it, draws one whole rect, then stops.
    SL_TEST_CASE(Stage3_StageRetryTick, Tag::Gpu)
    {
        printf("\n=== Stage 3: the simulated stage's retry tick ===\n");
        RigOptions options;
        options.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        options.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        auto rig = MakeDeferredRig(options);
        TEST("Stage3Tick_Built", rig.ok);
        if (!rig.ok) return;
        Dda::StageSim stage(rig.context.get(), rig.input.get(), rig.root.get(), rig.target.get(), g_d3dContext.get());
        stage.SetHosted(std::make_unique<Dda::DampedHostedDriver>(rig.stats.get(), L"Max"));
        stage.Prime();
        for (int i = 0; i < 8 && stage.TickDue(); ++i)
        {
            WaitForGpuIdle();
            stage.Tick();
        }
        const auto tickUntilIdle = [&](uint32_t& ticks, uint32_t& wholes, bool& hostedCommitted)
        {
            ticks = wholes = 0;
            hostedCommitted = false;
            for (int i = 0; i < 8 && stage.TickDue(); ++i)
            {
                WaitForGpuIdle();
                stage.Tick();
                ++ticks;
                wholes += stage.WholeDraws();
                hostedCommitted |= stage.HostedCommitted();
            }
        };

        // A Hosted ratchet the last present scheduled.
        Dda::DesktopFrame frame;
        rig.Fill(5, 5, 6, 6, cPeak2000);
        frame.dirty = { RECT{ 5, 5, 6, 6 } };
        const HRESULT present = stage.Present(frame);
        const bool due = stage.TickDue();
        uint32_t ticks = 0, wholes = 0;
        bool hostedCommitted = false;
        tickUntilIdle(ticks, wholes, hostedCommitted);
        printf("  [info] Hosted ratchet: present 0x%08X, %u tick(s), %u whole draw(s), committed %.2f\n",
               static_cast<unsigned>(present), ticks, wholes, rig.Committed(rig.maxIndex));
        TEST("Stage3Tick_HostedRatchetLandsByTick", present == S_FALSE && due && hostedCommitted && wholes == 1 &&
             !stage.TickDue() && Near(rig.Committed(rig.maxIndex), 2000.0f));

        // An engine (Threshold) commit after S_FALSE.
        rig.Fill(10, 10, 30, 30, cClipped);
        frame.dirty = { RECT{ 10, 10, 30, 30 } };
        const float budget0 = rig.Committed(rig.budgetIndex);
        const HRESULT present2 = stage.Present(frame);
        tickUntilIdle(ticks, wholes, hostedCommitted);
        printf("  [info] Threshold commit: present 0x%08X, %u tick(s), %u whole draw(s), budget %.6f -> %.6f\n",
               static_cast<unsigned>(present2), ticks, wholes, budget0, rig.Committed(rig.budgetIndex));
        TEST("Stage3Tick_EngineCommitLandsByTick", present2 == S_FALSE && ticks >= 1 && wholes == 1 &&
             !hostedCommitted && !stage.TickDue() && rig.Committed(rig.budgetIndex) > budget0 + 0.05f);
    }

    SL_TEST_CASE(Stage3_ReplayRecordings, Tag::Gpu)
    {
        printf("\n=== Stage 3: the simulated stage over DDA recordings ===\n");
        if (g_recordingPath.empty())
        {
            ReportNotRun("Stage3_ReplayRecordings: needs --recording <dir>");
            return;
        }
        std::error_code ec;
        uint32_t replayed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(g_recordingPath, ec))
        {
            if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / L"frames.bin")) continue;
            Dda::RecordingReader reader(entry.path());
            const std::string name = "Stage3Recording_" + entry.path().filename().string();
            if (!reader.Ok())
            {
                printf("  [info] %s: %s\n", name.c_str(), reader.Error().c_str());
                TEST((name + "_Readable").c_str(), false);
                continue;
            }
            if (RecordingIsVoid(entry.path(), name)) continue;
            ReplayResult* sink = nullptr;
            auto source = RecordingSource(reader, sink);
            ReportStage3(name, Stage3Replay(reader.Header().width, reader.Header().height, source));
            ++replayed;
        }
        TEST("Stage3Recording_AtLeastOneReplayed", replayed > 0);
    }
}

// ---- Stage 4: cell reductions -------------------------------------------
//
// The three statistics effects reduce over an 8x8 grid of cells whose
// partials persist, so a dispatch over rects recomputes only the cells they
// touch. The contract is bit identity: every field equals a dispatch that
// recomputed all 64 cells (incrementalReductions off) over the same pixels.
namespace
{
    constexpr const wchar_t* cCellEffects[] = { L"Luminance Statistics", L"Channel Statistics",
                                                L"Chromaticity Statistics" };

    // Every float of every field, in GetValues order; empty when it failed.
    std::vector<float> AllValues(IEffectGraphNode* node)
    {
        std::vector<float> all;
        winrt::com_ptr<IEffectGraphValues> values;
        if (FAILED(node->GetValues(values.put()))) return all;
        for (UINT32 i = 0; i < values->GetFieldCount(); ++i)
        {
            EFFECTGRAPH_FIELD_INFO info{};
            if (FAILED(values->GetFieldInfo(i, &info))) return {};
            const size_t start = all.size();
            all.resize(start + static_cast<size_t>(info.componentCount) * info.elementCount);
            if (FAILED(values->GetFloats(i, all.data() + start, static_cast<UINT32>(all.size() - start)))) return {};
        }
        return all;
    }

    bool SameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return !a.empty() && a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    Rendering::ReduceCellStats CellDelta(const Rendering::ReduceCellStats& after, const Rendering::ReduceCellStats& before)
    {
        return { after.dispatches - before.dispatches, after.incremental - before.incremental,
                 after.cellsDispatched - before.cellsDispatched, after.cellsTotal - before.cellsTotal };
    }

    // A texture input feeding `count` of the statistics effects, Luminance
    // Statistics' ClipNits bound to a Working Space. Readback skip off, so a
    // field read is the dispatch's own.
    struct CellArm
    {
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::com_ptr<IEffectGraphNode> input, workingSpace;
        std::vector<winrt::com_ptr<IEffectGraphNode>> stats;
        bool ok{ false };

        Rendering::GraphEvaluator& Evaluator() { return context->Evaluator(); }
        void InvalidateRects(const std::vector<RECT>& rects)
        {
            input.as<IEffectGraphInput2>()->InvalidateRects(rects.data(), static_cast<UINT32>(rects.size()));
        }
        void Invalidate() { input.as<IEffectGraphInput>()->Invalidate(); }
    };

    CellArm MakeCellArm(ID3D11Texture2D* texture, bool incremental, size_t count = std::size(cCellEffects))
    {
        CellArm arm;
        arm.context = CreateStageContext();
        auto options = arm.context->Evaluator().Options();
        options.incrementalReductions = incremental;
        options.skipUnneededCpuReadback = false;
        arm.context->Evaluator().SetOptions(options);
        auto& context = *arm.context;
        if (FAILED(context.CreateTextureInput(texture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, arm.input.put())) ||
            FAILED(context.CreateEffect(L"Working Space", arm.workingSpace.put())))
            return arm;
        for (size_t i = 0; i < count; ++i)
        {
            winrt::com_ptr<IEffectGraphNode> stats;
            if (FAILED(context.CreateEffect(cCellEffects[i], stats.put())) || FAILED(stats->SetInput(0, arm.input.get())))
                return arm;
            arm.stats.push_back(stats);
        }
        if (FAILED(arm.stats[0]->Bind(L"ClipNits", arm.workingSpace.get(), L"SdrWhiteNits"))) return arm;
        arm.ok = true;
        return arm;
    }

    struct Stage4Result
    {
        bool built{ false };
        uint32_t presents{ 0 }, rectPresents{ 0 }, failed{ 0 };
        // Presents on which Luminance Statistics consumed rects (the region
        // collapses to a bounding box past DirtyRegion::cMaxRects) touching
        // fewer than all 64 cells.
        uint32_t partialPresents{ 0 };
        // Of them, the presents on which Luminance Statistics itself
        // recomputed fewer than 64 cells (its own counters, not the
        // evaluator-wide ones), and the presents on which its dispatched
        // cells were not exactly the cells of the rects it consumed (all 64
        // for Whole), or it dispatched more than once.
        uint32_t luminancePartial{ 0 }, luminanceMaskWrong{ 0 };
        // Per effect: presents whose fields differ from the reference arm's,
        // and presents on which the fields moved.
        uint32_t mismatches[std::size(cCellEffects)]{}, moves[std::size(cCellEffects)]{};
        Rendering::ReduceCellStats cells, referenceCells;
        // The positive control: a change outside every reported rect.
        bool controlDiffers{ false }, controlRepaired{ false };
    };

    Stage4Result Stage4Replay(uint32_t width, uint32_t height, const FrameSource& source)
    {
        Stage4Result result;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        if (!source.start(desktop)) return result;
        auto incremental = MakeCellArm(desktop.Texture(), true);
        auto reference = MakeCellArm(desktop.Texture(), false);
        if (!incremental.ok || !reference.ok) return result;
        const size_t effects = std::size(cCellEffects);
        std::vector<std::vector<float>> last(effects);
        // A new effect needs two passes before its output is valid.
        for (int pass = 0; pass < 2; ++pass)
            for (size_t e = 0; e < effects; ++e)
            {
                last[e] = AllValues(incremental.stats[e].get());
                AllValues(reference.stats[e].get());
            }
        result.built = true;
        const uint32_t luminanceId = incremental.context->NodeId(incremental.stats[0].get());
        const auto cellsStart = incremental.Evaluator().ReduceCells();
        const auto referenceStart = reference.Evaluator().ReduceCells();
        const auto compareAll = [&](uint32_t* mismatches, uint32_t* moves)
        {
            bool same = true;
            for (size_t e = 0; e < effects; ++e)
            {
                const auto got = AllValues(incremental.stats[e].get());
                const auto want = AllValues(reference.stats[e].get());
                if (got.empty() || want.empty()) ++result.failed;
                if (!SameBits(got, want))
                {
                    same = false;
                    if (mismatches) ++mismatches[e];
                }
                if (moves && !SameBits(got, last[e])) ++moves[e];
                last[e] = got;
            }
            return same;
        };

        Dda::DesktopFrame frame;
        while (source.next(desktop, frame))
        {
            if (frame.kind == Dda::FrameKind::NoFrame) continue;
            ++result.presents;
            const auto changed = Dda::ChangedRects(frame);
            if (frame.whole)
            {
                incremental.Invalidate();
            }
            else
            {
                incremental.InvalidateRects(changed);
                if (!CoversSurface(changed, width, height)) ++result.rectPresents;
            }
            reference.Invalidate();
            const auto luminanceBefore = incremental.Evaluator().ReduceCells(luminanceId);
            compareAll(result.mismatches, result.moves);
            const auto luminance = incremental.Evaluator().ReduceCells(luminanceId);
            const auto luminanceDelta = CellDelta(luminance.totals, luminanceBefore.totals);
            if (luminanceDelta.dispatches == 0) continue;
            if (luminanceDelta.dispatches > 1)
            {
                ++result.luminanceMaskWrong;
                continue;
            }
            constexpr uint64_t all = ~uint64_t{ 0 };
            uint64_t expected = all;
            const auto region = incremental.Evaluator().DispatchInputRegion(luminanceId);
            if (region.IsRects())
            {
                std::vector<RECT> consumed;
                for (const auto& rect : region.Rects())
                    consumed.push_back({ rect.rect.left, rect.rect.top, rect.rect.right, rect.rect.bottom });
                expected = Rendering::D3D11ComputeRunner::CellMaskForRects(
                    consumed.data(), static_cast<uint32_t>(consumed.size()), width, height);
                // Nothing in view: the runner recomputes cell 0 to run the fold.
                if (expected == 0) expected = 1;
                if (expected != all) ++result.partialPresents;
                if (!luminance.lastRectsGiven) ++result.luminanceMaskWrong;
            }
            if (luminance.lastMask != expected) ++result.luminanceMaskWrong;
            if (luminanceDelta.incremental == 1) ++result.luminancePartial;
        }
        result.cells = CellDelta(incremental.Evaluator().ReduceCells(), cellsStart);
        result.referenceCells = CellDelta(reference.Evaluator().ReduceCells(), referenceStart);

        // Positive control: the last cell gets a peak no rect reports, then a
        // rect in the first cell is reported. The incremental arm keeps the
        // last cell's old partial, so it must differ from the reference.
        const uint32_t px = width - 1, py = height - 1;
        desktop.Put(px, py, 125.0f, 125.0f, 125.0f);
        desktop.Upload({ RECT{ static_cast<LONG>(px), static_cast<LONG>(py), static_cast<LONG>(width),
                               static_cast<LONG>(height) } });
        incremental.InvalidateRects({ RECT{ 0, 0, 1, 1 } });
        reference.Invalidate();
        result.controlDiffers = !compareAll(nullptr, nullptr);
        incremental.Invalidate();
        reference.Invalidate();
        result.controlRepaired = compareAll(nullptr, nullptr);
        return result;
    }

    void ReportStage4(const std::string& name, const Stage4Result& r)
    {
        printf("  [info] %s: %u presents (%u with rects; Luminance Statistics consumed rects touching fewer than 64 "
               "cells on %u), %u failed reads\n",
               name.c_str(), r.presents, r.rectPresents, r.partialPresents, r.failed);
        for (size_t e = 0; e < std::size(cCellEffects); ++e)
            printf("  [info] %s: %ls: %u of %u presents differ from the 64-cell reference; fields moved on %u\n",
                   name.c_str(), cCellEffects[e], r.mismatches[e], r.presents, r.moves[e]);
        printf("  [info] %s: Luminance Statistics recomputed fewer than 64 cells on %u presents; %u present(s) where its "
               "cells were not the consumed rects' cells\n", name.c_str(), r.luminancePartial, r.luminanceMaskWrong);
        printf("  [info] %s: incremental arm: %llu dispatches, %llu over fewer than 64 cells; ReduceCellsDispatched "
               "%llu of ReduceCellsTotal %llu (%.1f%%); reference arm %llu of %llu\n", name.c_str(),
               static_cast<unsigned long long>(r.cells.dispatches), static_cast<unsigned long long>(r.cells.incremental),
               static_cast<unsigned long long>(r.cells.cellsDispatched),
               static_cast<unsigned long long>(r.cells.cellsTotal),
               r.cells.cellsTotal ? 100.0 * r.cells.cellsDispatched / r.cells.cellsTotal : 0.0,
               static_cast<unsigned long long>(r.referenceCells.cellsDispatched),
               static_cast<unsigned long long>(r.referenceCells.cellsTotal));
        printf("  [info] %s: positive control (a change outside the rects) %s; repaired by a whole invalidate %d\n",
               name.c_str(), r.controlDiffers ? "differs, as it must" : "DOES NOT DIFFER: the comparison is blind",
               r.controlRepaired ? 1 : 0);
        TEST((name + "_Built").c_str(), r.built && r.presents > 0 && r.failed == 0);
        bool moved = true;
        for (size_t e = 0; e < std::size(cCellEffects); ++e)
        {
            TEST((name + "_BitIdentical_" + std::to_string(e)).c_str(), r.mismatches[e] == 0);
            moved = moved && r.moves[e] > 0;
        }
        // The instrument: the fields moved, the rects reached the reduction,
        // the reference recomputed everything, and the comparison sees a
        // stale cell.
        TEST((name + "_FieldsMoved").c_str(), moved);
        TEST((name + "_RectsReachedTheCells").c_str(),
             r.luminancePartial == r.partialPresents && r.luminanceMaskWrong == 0 &&
             r.cells.incremental >= r.partialPresents);
        TEST((name + "_ReferenceRecomputedEveryCell").c_str(),
             r.referenceCells.incremental == 0 && r.referenceCells.cellsDispatched == r.referenceCells.cellsTotal &&
             r.referenceCells.dispatches > 0);
        TEST((name + "_ControlDiffers").c_str(), r.controlDiffers && r.controlRepaired);
    }

    SL_TEST_CASE(Stage4_CellMaskForRects, Tag::Fast)
    {
        printf("\n=== Stage 4: the cells a rect touches ===\n");
        using Runner = Rendering::D3D11ComputeRunner;
        const auto mask = [](std::initializer_list<RECT> rects, uint32_t w, uint32_t h)
        {
            return Runner::CellMaskForRects(rects.begin(), static_cast<uint32_t>(rects.size()), w, h);
        };
        // 80 x 40: cells are 10 x 5.
        TEST("Stage4_Mask_None", Runner::CellMaskForRects(nullptr, 0, 80, 40) == 0);
        TEST("Stage4_Mask_OnePixel", mask({ RECT{ 0, 0, 1, 1 } }, 80, 40) == 1);
        TEST("Stage4_Mask_LastPixel", mask({ RECT{ 79, 39, 80, 40 } }, 80, 40) == (uint64_t{ 1 } << 63));
        TEST("Stage4_Mask_EdgeIsExclusive", mask({ RECT{ 0, 0, 10, 5 } }, 80, 40) == 1);
        TEST("Stage4_Mask_AcrossAnEdge", mask({ RECT{ 9, 4, 11, 6 } }, 80, 40) == (1ull | 2ull | (1ull << 8) | (1ull << 9)));
        TEST("Stage4_Mask_Row", mask({ RECT{ 0, 0, 80, 1 } }, 80, 40) == 0xFFull);
        TEST("Stage4_Mask_Whole", mask({ RECT{ -5, -5, 500, 500 } }, 80, 40) == ~uint64_t{ 0 });
        TEST("Stage4_Mask_OutsideIsEmpty", mask({ RECT{ 80, 0, 90, 10 }, RECT{ 5, 5, 5, 9 } }, 80, 40) == 0);
        TEST("Stage4_Mask_Union", mask({ RECT{ 0, 0, 1, 1 }, RECT{ 79, 39, 80, 40 } }, 80, 40) ==
                                      (1ull | (uint64_t{ 1 } << 63)));
        // Not a multiple of 8: 13 x 3. Columns end at 1,3,4,6,8,9,11,13;
        // rows 0 and 1 are empty (y1 = 0), so pixel row 0 is row 2's.
        TEST("Stage4_Mask_UnevenGrid", mask({ RECT{ 0, 0, 1, 1 } }, 13, 3) == (uint64_t{ 1 } << 16));
        TEST("Stage4_Mask_UnevenGridColumn", mask({ RECT{ 12, 2, 13, 3 } }, 13, 3) == (uint64_t{ 1 } << 63));
    }

    SL_TEST_CASE(Stage4_ReplaySynthetic, Tag::Gpu)
    {
        printf("\n=== Stage 4: incremental cell reductions against 64-cell ones, synthetic scenarios ===\n");
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        std::vector<Dda::ScenarioKind> kinds(std::begin(cScenarios), std::end(cScenarios));
        kinds.push_back(Dda::ScenarioKind::HdrHighlights);
        uint32_t partial = 0;
        for (const auto kind : kinds)
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = Stage4Replay(width, height, SyntheticSource(*scenario));
            ReportStage4(std::string("Stage4_") + Dda::ScenarioName(kind), result);
            partial += result.luminancePartial;
        }
        // Luminance Statistics recomputed fewer than 64 cells somewhere in the replay.
        TEST("Stage4_ReplayRanIncremental", partial > 0);
    }

    SL_TEST_CASE(Stage4_ReplayRecordings, Tag::Gpu)
    {
        printf("\n=== Stage 4: incremental cell reductions over DDA recordings ===\n");
        if (g_recordingPath.empty())
        {
            ReportNotRun("Stage4_ReplayRecordings: needs --recording <dir>");
            return;
        }
        std::error_code ec;
        uint32_t replayed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(g_recordingPath, ec))
        {
            if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / L"frames.bin")) continue;
            Dda::RecordingReader reader(entry.path());
            const std::string name = "Stage4Recording_" + entry.path().filename().string();
            if (!reader.Ok())
            {
                printf("  [info] %s: %s\n", name.c_str(), reader.Error().c_str());
                TEST((name + "_Readable").c_str(), false);
                continue;
            }
            if (RecordingIsVoid(entry.path(), name)) continue;
            ReplayResult* sink = nullptr;
            ReportStage4(name, Stage4Replay(reader.Header().width, reader.Header().height, RecordingSource(reader, sink)));
            ++replayed;
        }
        TEST("Stage4Recording_AtLeastOneReplayed", replayed > 0);
    }

    // The paths where rects reach a statistics node but no dispatch consumes
    // them in that frame, and the switch that turns the cells off.
    SL_TEST_CASE(Stage4_Recovery, Tag::Gpu)
    {
        printf("\n=== Stage 4: rects that wait for a later dispatch ===\n");
        constexpr uint32_t width = 96, height = 48;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float level = 0.1f + 3.0f * static_cast<float>(x + y) / (width + height);
                desktop.Put(x, y, level, level * 0.8f, level * 1.1f);
            }
        desktop.UploadAll();
        auto incremental = MakeCellArm(desktop.Texture(), true);
        auto reference = MakeCellArm(desktop.Texture(), false);
        TEST("Stage4Recovery_Built", incremental.ok && reference.ok);
        if (!incremental.ok || !reference.ok) return;
        auto& model = incremental.context->Model();
        auto& evaluator = incremental.Evaluator();
        const uint32_t inputId = incremental.context->NodeId(incremental.input.get());
        const uint32_t statsId = incremental.context->NodeId(incremental.stats[0].get());
        // Every effect of both arms; true when all are bit-identical.
        const auto matches = [&]
        {
            reference.Invalidate();
            bool same = true;
            for (size_t e = 0; e < incremental.stats.size(); ++e)
                same = SameBits(AllValues(incremental.stats[e].get()), AllValues(reference.stats[e].get())) && same;
            return same;
        };
        // Paints `rect` with a new level, so the fields move.
        float level = 6.0f;
        const auto paint = [&](const RECT& rect)
        {
            level += 1.5f;
            for (LONG y = rect.top; y < rect.bottom; ++y)
                for (LONG x = rect.left; x < rect.right; ++x)
                    desktop.Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), level, level, level);
            desktop.Upload({ rect });
        };
        for (int pass = 0; pass < 2; ++pass) matches();
        TEST("Stage4Recovery_Primed", matches());

        // Rects while the input has no output: a frame driven by hand with the
        // input's image withheld, as a host input not ready yet would be. The
        // node cannot queue, keeps the rects, and the next frame dispatches
        // them with nothing new.
        {
            const RECT rect{ 3, 3, 9, 7 };
            const auto before = AllValues(incremental.stats[0].get());
            paint(rect);
            incremental.InvalidateRects({ rect });
            const auto cellsBefore = evaluator.ReduceCells();
            auto* dc = incremental.context->DeviceContext();
            model.PropagateDirtyDownstream(true);
            model.MarkAllNeeded();
            model.FindNode(inputId)->cachedOutput = nullptr;
            evaluator.Evaluate(model, dc);
            evaluator.ProcessDeferredCompute(model, dc);
            const bool held = CellDelta(evaluator.ReduceCells(), cellsBefore).dispatches == 0;
            const auto recoveredCells = evaluator.ReduceCells();
            const auto after = AllValues(incremental.stats[0].get());
            const uint32_t recoveryDispatches = evaluator.DispatchesLastFrame();
            const auto delta = CellDelta(evaluator.ReduceCells(), recoveredCells);
            const auto region = evaluator.DispatchInputRegion(statsId);
            printf("  [info] input without output: held %d; the next frame dispatched %llu time(s), %llu cell(s), over %s\n",
                   held ? 1 : 0, static_cast<unsigned long long>(delta.dispatches),
                   static_cast<unsigned long long>(delta.cellsDispatched),
                   region.IsWhole() ? "Whole" : region.IsNone() ? "None" : "the rects");
            TEST("Stage4Recovery_NoOutputHeldTheRects", held);
            TEST("Stage4Recovery_NoOutputRecoveredWithRects",
                 recoveryDispatches >= 1 && delta.dispatches >= 1 && delta.incremental >= 1 && !region.IsWhole() &&
                 !region.IsNone());
            TEST("Stage4Recovery_NoOutputFieldsMoved", !SameBits(before, after));
            TEST("Stage4Recovery_NoOutputEqualsForcedWhole", matches());
        }

        // A dropped queue (a Source with no output), then a frame with no new
        // change: the rects re-queue the node, Whole.
        {
            const RECT rect{ 40, 20, 52, 30 };
            paint(rect);
            const uint32_t emptyId = model.AddNode(Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Empty"));
            incremental.InvalidateRects({ rect });
            const auto cellsBefore = evaluator.ReduceCells();
            AllValues(incremental.stats[0].get());
            const bool dropped = CellDelta(evaluator.ReduceCells(), cellsBefore).dispatches == 0;
            model.RemoveNode(emptyId);
            const auto recoveredCells = evaluator.ReduceCells();
            AllValues(incremental.stats[0].get());
            const uint32_t recoveryDispatches = evaluator.DispatchesLastFrame();
            const bool same = matches();
            const auto delta = CellDelta(evaluator.ReduceCells(), recoveredCells);
            printf("  [info] dropped queue: dropped %d; the clean frame dispatched %llu time(s), %llu cell(s)\n",
                   dropped ? 1 : 0, static_cast<unsigned long long>(delta.dispatches),
                   static_cast<unsigned long long>(delta.cellsDispatched));
            TEST("Stage4Recovery_QueueDropped", dropped);
            TEST("Stage4Recovery_DroppedRequeuedWhole", recoveryDispatches >= 1 && delta.dispatches >= 1 &&
                 delta.cellsDispatched == delta.cellsTotal && evaluator.DispatchInputRegion(statsId).IsWhole());
            TEST("Stage4Recovery_DroppedEqualsForcedWhole", same);
        }

        // A failed dispatch leaves the node in error: nothing re-queues it
        // until RetryNode, which dispatches Whole.
        {
            const RECT rect{ 70, 5, 80, 15 };
            paint(rect);
            incremental.InvalidateRects({ rect });
            evaluator.FailNextComputeDispatchForTest(true);
            AllValues(incremental.stats[0].get());
            // The first dispatch of the frame failed: whichever node that was.
            uint32_t failedId = 0;
            for (const auto& stats : incremental.stats)
                if (const uint32_t id = incremental.context->NodeId(stats.get()); !model.FindNode(id)->runtimeError.empty())
                    failedId = failedId ? UINT32_MAX : id;
            const bool failed = !evaluator.FailNextComputeDispatchArmed() && failedId && failedId != UINT32_MAX;
            evaluator.FailNextComputeDispatchForTest(false);
            const auto idleBefore = evaluator.ReduceCells();
            AllValues(incremental.stats[0].get());
            const auto idle = CellDelta(evaluator.ReduceCells(), idleBefore);
            const bool retried = failed && Graph::RetryNode(model, failedId);
            const auto retryBefore = evaluator.ReduceCells();
            const bool same = matches();
            const auto retry = CellDelta(evaluator.ReduceCells(), retryBefore);
            printf("  [info] failed dispatch: failed %d; %llu dispatch(es) while in error; RetryNode %d -> %llu dispatch(es), "
                   "%llu cell(s)\n", failed ? 1 : 0, static_cast<unsigned long long>(idle.dispatches), retried ? 1 : 0,
                   static_cast<unsigned long long>(retry.dispatches), static_cast<unsigned long long>(retry.cellsDispatched));
            TEST("Stage4Recovery_DispatchFailed", failed);
            TEST("Stage4Recovery_ErrorNotRequeued", idle.dispatches == 0);
            TEST("Stage4Recovery_RetryWhole", retried && retry.dispatches >= 1 && retry.incremental == 0 &&
                 model.FindNode(failedId)->runtimeError.empty());
            TEST("Stage4Recovery_RetryEqualsForcedWhole", same);
        }

        // ClipNits, bound to Working Space, moves in the same frame as a rect:
        // Luminance Statistics recomputes every cell; the others only the rect's.
        {
            const RECT rect{ 20, 30, 24, 34 };
            paint(rect);
            auto profile = Rendering::PresetP3_1000();
            profile.caps.sdrWhiteLevelNits = 300.0f;
            incremental.context->SetDisplayProfile(profile);
            reference.context->SetDisplayProfile(profile);
            incremental.InvalidateRects({ rect });
            const auto before = evaluator.ReduceCells();
            const auto luminance = AllValues(incremental.stats[0].get());
            const auto luminanceCells = CellDelta(evaluator.ReduceCells(), before);
            // The evaluator itself must hand Luminance Statistics no rects:
            // the runner's key would also catch a moved cbuffer constant, but
            // not a parameter routed to the GPU outside the cbuffer.
            const auto luminanceNode = evaluator.ReduceCells(statsId);
            bool othersGotRects = true;
            const uint64_t rectMask = Rendering::D3D11ComputeRunner::CellMaskForRects(&rect, 1, width, height);
            for (size_t e = 1; e < incremental.stats.size(); ++e)
            {
                const auto other = evaluator.ReduceCells(incremental.context->NodeId(incremental.stats[e].get()));
                othersGotRects = othersGotRects && other.lastRectsGiven && other.lastMask == rectMask;
            }
            const bool same = matches();
            printf("  [info] ClipNits moved with a rect: %llu of %llu cells recomputed (64 by Luminance Statistics)\n",
                   static_cast<unsigned long long>(luminanceCells.cellsDispatched),
                   static_cast<unsigned long long>(luminanceCells.cellsTotal));
            // The other two dispatch over the rect's one cell.
            TEST("Stage4Recovery_ClipNitsMovedRecomputesEveryCell", !luminance.empty() &&
                 luminanceCells.dispatches - luminanceCells.incremental == 1 &&
                 luminanceCells.cellsDispatched == 64 + luminanceCells.incremental);
            TEST("Stage4Recovery_ClipNitsMovedEvaluatorPassedWhole", !luminanceNode.lastRectsGiven &&
                 luminanceNode.lastMask == ~uint64_t{ 0 } && evaluator.DispatchInputRegion(statsId).IsWhole());
            TEST("Stage4Recovery_ClipNitsMovedOthersGotTheRect", othersGotRects);
            TEST("Stage4Recovery_ClipNitsMovedEqualsForcedWhole", same);
        }

        // A dispatch whose command list never runs fails its node; the retry
        // recomputes every cell, and the result equals a forced-whole one.
        {
            const RECT rect{ 60, 12, 62, 14 };
            paint(rect);
            incremental.InvalidateRects({ rect });
            evaluator.FailNextComputeSubmitForTest(statsId);
            AllValues(incremental.stats[0].get());
            const bool failed = !model.FindNode(statsId)->runtimeError.empty();
            const bool retried = failed && Graph::RetryNode(model, statsId);
            const auto retryBefore = evaluator.ReduceCells(statsId);
            const bool same = matches();
            const auto retry = evaluator.ReduceCells(statsId);
            printf("  [info] lost submit: node failed %d; RetryNode %d -> %llu dispatch(es), mask %016llx\n",
                   failed ? 1 : 0, retried ? 1 : 0,
                   static_cast<unsigned long long>(retry.totals.dispatches - retryBefore.totals.dispatches),
                   static_cast<unsigned long long>(retry.lastMask));
            TEST("Stage4Recovery_LostSubmitFailsTheNode", failed);
            TEST("Stage4Recovery_LostSubmitRetryWhole", retried && retry.totals.dispatches - retryBefore.totals.dispatches == 1 &&
                 retry.lastMask == ~uint64_t{ 0 } && model.FindNode(statsId)->runtimeError.empty());
            TEST("Stage4Recovery_LostSubmitEqualsForcedWhole", same);
        }

        // The kill switch: on, a one-pixel rect recomputes its one cell (44,
        // not cell 0, which an empty mask also dispatches); off, all 64.
        {
            const RECT rect{ 50, 30, 51, 31 };
            constexpr uint64_t cell44 = uint64_t{ 1 } << 44;
            paint(rect);
            incremental.InvalidateRects({ rect });
            auto before = evaluator.ReduceCells();
            AllValues(incremental.stats[0].get());
            const auto on = CellDelta(evaluator.ReduceCells(), before);
            bool onCell44 = true;
            for (const auto& stats : incremental.stats)
            {
                const auto node = evaluator.ReduceCells(incremental.context->NodeId(stats.get()));
                onCell44 = onCell44 && node.lastRectsGiven && node.lastMask == cell44;
            }
            const bool onSame = matches();
            auto options = evaluator.Options();
            options.incrementalReductions = false;
            evaluator.SetOptions(options);
            paint(rect);
            incremental.InvalidateRects({ rect });
            before = evaluator.ReduceCells();
            AllValues(incremental.stats[0].get());
            const auto off = CellDelta(evaluator.ReduceCells(), before);
            printf("  [info] kill switch: a one-pixel rect recomputes %llu cell(s) on, %llu off\n",
                   static_cast<unsigned long long>(on.cellsDispatched), static_cast<unsigned long long>(off.cellsDispatched));
            TEST("Stage4Recovery_SwitchOnRecomputesOneCell", on.dispatches >= 1 && on.cellsDispatched == on.dispatches &&
                 onCell44);
            TEST("Stage4Recovery_SwitchOnEqualsForcedWhole", onSame);
            TEST("Stage4Recovery_SwitchOffRecomputesEveryCell",
                 off.dispatches >= 1 && off.incremental == 0 && off.cellsDispatched == 64 * off.dispatches);
            TEST("Stage4Recovery_SwitchOffEqualsForcedWhole", matches());
            TEST("Stage4Recovery_DefaultOn", Rendering::EvaluatorOptions{}.incrementalReductions);
        }
    }

    // The runner on its own: a dispatch whose command list never reaches the
    // GPU leaves the cells' partials as they were, so the next dispatch must
    // not trust them, whatever rects it is handed.
    SL_TEST_CASE(Stage4_RunnerLostSubmit, Tag::Gpu)
    {
        printf("\n=== Stage 4: a cell reduction whose submit is lost ===\n");
        // Each cell stores the sum of its pixels' red; the last group folds.
        const char* const cHlsl = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
SHADERLAB_REDUCE_CELLS
cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
};
groupshared uint gs_sum;
[numthreads(8, 8, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * 8;
    uint cell = ShaderLabReduceCell(Gid.x);
    uint4 r = ShaderLabCellRect(cell, Width, Height);
    if (tid == 0) gs_sum = 0;
    GroupMemoryBarrierWithGroupSync();
    uint local = 0;
    for (uint y = r.y + GTid.y; y < r.w; y += 8)
        for (uint x = r.x + GTid.x; x < r.z; x += 8)
            local += (uint)Source.Load(int3(x, y, 0)).r;
    InterlockedAdd(gs_sum, local);
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(cell, 0), gs_sum);
    if (!ShaderLabReduceIsLastGroup(tid)) return;
    if (tid == 0)
    {
        uint total = 0;
        for (uint s = 0; s < SlotCount; ++s) total += ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(s, 0));
        Result[0] = float4(float(total), 0, 0, 0);
    }
}
)HLSL";
        using Runner = Rendering::D3D11ComputeRunner;
        Runner runner;
        runner.Initialize(g_d3dDevice.get());
        const bool compiled = runner.CompileShader(cHlsl);
        if (!compiled) printf("    (debug) compile: %ls\n", runner.GetCompileError().c_str());
        TEST("Stage4Runner_Compiled", compiled && runner.UsesReduceCells());
        if (!compiled || !runner.UsesReduceCells()) return;

        // 64 x 64: cells are 8 x 8, so pixel (8 cx, 8 cy) is in cell 8 cy + cx.
        constexpr uint32_t size = 64;
        std::vector<float> zeros(size * size * 4, 0.0f);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init{ zeros.data(), size * 16, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, &init, texture.put()));
        const auto light = [&](uint32_t cell) -> RECT
        {
            const UINT x = (cell % 8) * 8, y = (cell / 8) * 8;
            const float one[4]{ 1.0f, 0.0f, 0.0f, 1.0f };
            const D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
            g_d3dContext->UpdateSubresource(texture.get(), 0, &box, one, 16, 16);
            return RECT{ static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 1), static_cast<LONG>(y + 1) };
        };
        const auto run = [&](const RECT* rect)
        {
            runner.SetCellRects(rect, rect ? 1 : 0);
            const auto values = runner.Dispatch(texture.get(), {}, 1);
            return values.empty() ? -1.0f : values[0];
        };

        const float first = run(nullptr);
        const bool wholeFirst = runner.LastCellsDispatched() == 64;
        const RECT r10 = light(10);
        const float afterTen = run(&r10);
        const bool tenOnly = runner.LastCellMask() == (uint64_t{ 1 } << 10);
        // Cell 20 changes, but its dispatch never runs.
        const RECT r20 = light(20);
        runner.FailNextSubmitForTest();
        run(&r20);
        const bool lost = FAILED(runner.LastDispatchResult());
        // Only cell 30's rect is reported next: the runner must not trust
        // cell 20's kept partial.
        const RECT r30 = light(30);
        const float afterLost = run(&r30);
        const uint32_t recomputed = runner.LastCellsDispatched();
        // And then it is incremental again.
        const RECT r40 = light(40);
        const float afterForty = run(&r40);
        const bool fortyOnly = runner.LastCellMask() == (uint64_t{ 1 } << 40);
        printf("  [info] sums: %.0f, %.0f, lost submit (failed %d), %.0f over %u cell(s), %.0f\n", first, afterTen,
               lost ? 1 : 0, afterLost, recomputed, afterForty);
        TEST("Stage4Runner_Incremental", first == 0.0f && wholeFirst && afterTen == 1.0f && tenOnly);
        TEST("Stage4Runner_LostSubmitFails", lost);
        TEST("Stage4Runner_LostSubmitRecomputesEveryCell", recomputed == 64 && afterLost == 3.0f);
        TEST("Stage4Runner_IncrementalAfterRecovery", fortyOnly && afterForty == 4.0f);
    }

    // A one-pixel rect per cell row/column pattern covering `cells` cells of
    // a width x height image (1, 2, 4, 8, 16, 32).
    std::vector<RECT> RectsForCells(uint32_t cells, uint32_t width, uint32_t height)
    {
        const LONG cw = static_cast<LONG>(width / 8), ch = static_cast<LONG>(height / 8), w = static_cast<LONG>(width);
        switch (cells)
        {
        case 1: return { RECT{ 3 * cw + 1, 5 * ch + 1, 3 * cw + 2, 5 * ch + 2 } };   // cell 43, not cell 0
        case 2: return { RECT{ 0, 0, cw + 1, 1 } };
        case 4: return { RECT{ 0, 0, cw + 1, ch + 1 } };
        case 8: return { RECT{ 0, 0, w, 1 } };
        case 16: return { RECT{ 0, 0, w, ch + 1 } };
        case 32: return { RECT{ 0, 0, w, 3 * ch + 1 } };
        default: return {};
        }
    }

    // The interleaved 64-group reduction Luminance Statistics ran before the
    // cells (version 6): the cost baseline for a whole dispatch.
    const char* const cLuminanceStatisticsV6Hlsl = R"HLSL(
// Luminance Statistics - D3D11 Compute Shader
// Outputs (in nits when Units = 1, normalized otherwise):
//   Min, Max, Mean, Median, P95, P99, AvgLog, ClippedFraction, Samples,
//   WhiteErrorBudget, HighlightP99
//
// HighlightP99 is the 99th percentile of the above-white pixels only, so it
// describes the highlights whether they fill the frame or one small window.
//
// WhiteErrorBudget = min(ClippedFraction * BudgetScale, BudgetMax), computed
// HERE rather than in a downstream Numeric Expression node: that node
// evaluates on the CPU, so binding it to ClippedFraction would force this
// node's analysis buffer through a GPU->CPU Map() every frame, and the Map
// waits on the GPU. Producing the value on the GPU keeps it GPU-resident.
//
// Units enum:
//   0 = Normalized (scRGB Y, where 1.0 = SDR white = 80 nits)
//   1 = Nits (Y * 80)
//
// ClippedFraction: fraction of pixels whose luminance > ClipNits * CLIP_MARGIN.
// The margin keeps SDR white (W/80, a few FP16 / dot-product ulps above W)
// from counting; 1% above white is under 0.8 dE ITP from it.

// Multi-group reduction (SHADERLAB_REDUCE_SCRATCH): SHADERLAB_REDUCE_GROUPS
// groups each take every G-th row and write a partial block; the last group
// to finish folds them, so the memory-bound scan is spread over many CUs.

#include "shaderlab_params.hlsli"

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Units;        // 0 = Normalized, 1 = Nits
    float ClipNits;
    float BudgetScale; // dE ITP of white error to spend per unit clipped
    float BudgetMax;   // ceiling, matching WhiteErrorBudget's own max
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256
#define CLIP_MARGIN 1.01

// Log-spaced histogram covers 8 decades: 1e-2 .. 1e6 nits.
// nits = 10^(LOG_MIN + (bin / HIST_BINS) * LOG_RANGE)
#define LOG_MIN   -2.0
#define LOG_RANGE 8.0

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared float gs_logsum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_clipped[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

void Combine(uint dst, uint src)
{
    gs_min[dst] = min(gs_min[dst], gs_min[src]);
    gs_max[dst] = max(gs_max[dst], gs_max[src]);
    gs_sum[dst] += gs_sum[src];
    gs_logsum[dst] += gs_logsum[src];
    gs_count[dst] += gs_count[src];
    gs_clipped[dst] += gs_clipped[src];
}

// The luminance at a log-histogram bin's centre, in nits.
float BinNits(uint bin)
{
    return pow(10.0, LOG_MIN + (float(bin) + 0.5) / float(HIST_BINS) * LOG_RANGE);
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;
    uint g = Gid.x;

    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Clip threshold in nits.
    float clipThreshold = ClipNits * CLIP_MARGIN;

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;       // sum of nit values
    float tLogSum = 0;    // sum of log10(nits) for AvgLog
    uint  tCount = 0;
    uint  tClipped = 0;

    for (uint y = g; y < Height; y += SHADERLAB_REDUCE_GROUPS)
    {
        for (uint x = tid; x < Width; x += THREAD_COUNT)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float Y = max(0.0, dot(pix.rgb, float3(0.2126, 0.7152, 0.0722)));
            float nits = Y * 80.0;

            tCount++;
            tMin = min(tMin, nits);
            tMax = max(tMax, nits);
            tSum += nits;
            float lv = log10(max(nits, 1e-4));
            tLogSum += lv;
            if (nits > clipThreshold) tClipped++;

            float t = (lv - LOG_MIN) / LOG_RANGE;
            uint bin = clamp((uint)(saturate(t) * (HIST_BINS - 1)), 0u, (uint)(HIST_BINS - 1));
            InterlockedAdd(gs_hist[bin], 1u);
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_logsum[tid] = tLogSum;
    gs_count[tid] = tCount;
    gs_clipped[tid] = tClipped;
    ShaderLabTreeReduce(tid, THREAD_COUNT, Combine)

    if (tid == 0)
    {
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 0), asuint(gs_min[0]));
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 1), asuint(gs_max[0]));
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 2), asuint(gs_sum[0]));
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 3), asuint(gs_logsum[0]));
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 4), gs_count[0]);
        ShaderLabScratchStore(SHADERLAB_PARTIAL_WORD(g, 5), gs_clipped[0]);
    }
    ShaderLabStoreHistogram(gs_hist, HIST_BINS, g, tid, THREAD_COUNT)

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    // Last group: fold every group's partials; idle threads hold the identity.
    if (tid < SHADERLAB_REDUCE_GROUPS)
    {
        gs_min[tid]     = asfloat(ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 0)));
        gs_max[tid]     = asfloat(ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 1)));
        gs_sum[tid]     = asfloat(ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 2)));
        gs_logsum[tid]  = asfloat(ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 3)));
        gs_count[tid]   = ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 4));
        gs_clipped[tid] = ShaderLabScratchLoad(SHADERLAB_PARTIAL_WORD(tid, 5));
    }
    else
    {
        gs_min[tid] = 1e30; gs_max[tid] = -1e30; gs_sum[tid] = 0;
        gs_logsum[tid] = 0; gs_count[tid] = 0; gs_clipped[tid] = 0;
    }
    ShaderLabMergeHistograms(gs_hist, HIST_BINS, tid, THREAD_COUNT)
    ShaderLabTreeReduce(tid, THREAD_COUNT, Combine)

    if (tid == 0)
    {
        uint  totalSamples = gs_count[0];
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        float fMean   = (totalSamples > 0) ? (gs_sum[0]    / float(totalSamples)) : 0.0;
        float fAvgLogLog = (totalSamples > 0) ? (gs_logsum[0] / float(totalSamples)) : 0.0;
        float fAvgLog = pow(10.0, fAvgLogLog);  // geometric mean in nits

        // Histogram CDF -> percentiles (in log-domain bin centres).
        uint medianBin, p95Bin, p99Bin;
        ShaderLabPercentileBin(gs_hist, HIST_BINS, totalSamples / 2, medianBin)
        ShaderLabPercentileBin(gs_hist, HIST_BINS, (uint)(totalSamples * 0.95), p95Bin)
        ShaderLabPercentileBin(gs_hist, HIST_BINS, (uint)(totalSamples * 0.99), p99Bin)
        float fMedian = (medianBin < HIST_BINS) ? BinNits(medianBin) : 0.0;
        float fP95    = (p95Bin    < HIST_BINS) ? BinNits(p95Bin)    : 0.0;
        float fP99    = (p99Bin    < HIST_BINS) ? BinNits(p99Bin)    : 0.0;

        float clippedFrac = (totalSamples > 0)
            ? float(gs_clipped[0]) / float(totalSamples) : 0.0;

        // Convert to normalized (scRGB Y) if requested.
        float scale = (Units == 1) ? 1.0 : (1.0 / 80.0);

        Result[SL_FIELD_Min] = float4(fMin    * scale, 0, 0, 0);
        Result[SL_FIELD_Max] = float4(fMax    * scale, 0, 0, 0);
        Result[SL_FIELD_Mean] = float4(fMean   * scale, 0, 0, 0);
        Result[SL_FIELD_Median] = float4(fMedian * scale, 0, 0, 0);
        Result[SL_FIELD_P95] = float4(fP95    * scale, 0, 0, 0);
        Result[SL_FIELD_P99] = float4(fP99    * scale, 0, 0, 0);
        Result[SL_FIELD_AvgLog] = float4(fAvgLog * scale, 0, 0, 0);
        Result[SL_FIELD_ClippedFraction] = float4(clippedFrac, 0, 0, 0);
        Result[SL_FIELD_Samples] = float4(float(totalSamples), 0, 0, 0);
        // Adaptive white-error budget (a dE ITP allowance that grows with
        // the clipped fraction), ready to bind straight into a consumer's
        // gpu-bindable parameter with nothing CPU-side in between. Clamped here so a consumer cannot be handed a value
        // outside the parameter's declared range.
        Result[SL_FIELD_WhiteErrorBudget] = float4(
            clamp(min(clippedFrac * BudgetScale, BudgetMax), 0.0, BudgetMax),
            0, 0, 0);

        // HighlightP99: the 99th percentile of the pixels ClippedFraction
        // counts, walked down from the top bin. With nothing above white it
        // is the clip threshold itself.
        float fHighlightP99 = clipThreshold;
        const uint highlightTail = gs_clipped[0] / 100;
        uint above = 0;
        if (gs_clipped[0] > 0)
        {
            for (int hb2 = HIST_BINS - 1; hb2 >= 0; --hb2)
            {
                above += gs_hist[hb2];
                if (above > highlightTail)
                {
                    fHighlightP99 = max(BinNits(hb2), clipThreshold);
                    break;
                }
            }
        }
        Result[SL_FIELD_HighlightP99] = float4(fHighlightP99 * scale, 0, 0, 0);
    }
}
)HLSL";

    // ComparePaired, the timing comparisons' instrument, on known samples.
    SL_TEST_CASE(TestPairedTiming, Tag::Fast)
    {
        printf("\n=== Paired timing comparison ===\n");
        // 21 pairs with differences -10..10: the 99% sign-test interval of
        // n = 21 is [d(5), d(17)] (P(X <= 4) = 0.0036), so -6..6.
        std::vector<double> a, b;
        for (int i = 0; i < 21; ++i)
        {
            a.push_back(100.0 + (i - 10));
            b.push_back(100.0);
        }
        const auto even = ComparePaired(a, b);
        TEST("Paired_Median", even.pairs == 21 && even.median == 0.0);
        TEST("Paired_Interval", even.lower == -6.0 && even.upper == 6.0);
        TEST("Paired_NeitherSide", !even.ConfidentlyAbove() && !even.ConfidentlyBelow());
        TEST("Paired_RatioAndMargin", ComparePaired(a, b, 0.5, 10.0).ConfidentlyAbove() &&
                                      ComparePaired(a, b, 1.0, 8.0).ConfidentlyBelow());
        // A drift both arms share cancels in the pairs.
        std::vector<double> c, d;
        for (int i = 0; i < 21; ++i)
        {
            const double clock = (i % 3 == 0) ? 2.0 : 1.0;
            c.push_back(1.1 * clock);
            d.push_back(1.0 * clock);
        }
        TEST("Paired_DriftCancels", ComparePaired(c, d).ConfidentlyAbove() && !ComparePaired(c, d, 1.15).ConfidentlyAbove());
        TEST("Paired_FewPairsWholeRange", ComparePaired({ 1.0, 2.0, 3.0 }, { 0.0, 0.0, 0.0 }).lower == 1.0);
        TEST("Paired_Empty", ComparePaired({}, {}).pairs == 0 && !ComparePaired({}, {}).ConfidentlyAbove());
    }

    // GPU cost of a Luminance Statistics frame by the cells it recomputes, on
    // an FP16 desktop-sized input, from an idle GPU.
    SL_TEST_CASE(Stage4_CellCost, Tag::Gpu)
    {
        printf("\n=== Stage 4: the cost of a cell reduction by cells recomputed ===\n");
        const uint32_t width = g_useWarp ? 480 : 3840, height = g_useWarp ? 272 : 2160;
        const int reps = g_useWarp ? 5 : 21;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                // A ramp to 400 nits with a 1000-nit block: content the
                // percentile histograms have to sort.
                const float nits = (x > width / 3 && x < width / 2 && y > height / 3 && y < height / 2)
                                       ? 1000.0f : 1.0f + 399.0f * static_cast<float>((x * 7 + y * 3) % width) / width;
                desktop.Put(x, y, nits / 80.0f, nits / 80.0f, nits / 80.0f);
            }
        desktop.UploadAll();

        GpuStopwatch stopwatch;
        struct Arm
        {
            winrt::com_ptr<IEffectGraphContextInternal> context;
            winrt::com_ptr<IEffectGraphNode> input, stats;
            uint32_t statsId{ 0 };
        };
        // Readback skip on (the stage's context): the frame is GPU work only.
        const auto makeArm = [&](ID3D11Texture2D* texture, bool legacy)
        {
            Arm arm;
            arm.context = CreateStageContext();
            winrt::check_hresult(arm.context->CreateTextureInput(texture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                 arm.input.put()));
            if (legacy)
            {
                auto descriptor = *Effects::ShaderLabEffects::Instance().FindByName(L"Luminance Statistics");
                descriptor.name = descriptor.effectId = L"Luminance Statistics v6 (interleaved)";
                descriptor.effectVersion = 6;
                descriptor.hlslSource = cLuminanceStatisticsV6Hlsl;
                descriptor.dispatchShape = Graph::DispatchShape::ReduceGroups;
                const uint32_t id = arm.context->Model().AddNode(Effects::ShaderLabEffects::CreateNode(descriptor));
                winrt::check_hresult(arm.context->NodeFor(id, arm.stats.put()));
            }
            else
            {
                winrt::check_hresult(arm.context->CreateEffect(L"Luminance Statistics", arm.stats.put()));
            }
            winrt::check_hresult(arm.stats->SetInput(0, arm.input.get()));
            arm.statsId = arm.context->NodeId(arm.stats.get());
            AllValues(arm.stats.get());
            AllValues(arm.stats.get());
            return arm;
        };
        // GPU ms of one frame realizing the statistics after `invalidate`;
        // `wrong` counts frames that did not dispatch the statistics once,
        // over exactly `mask`'s cells (`mask` 0: not a cell reduction, so the
        // frame's dispatch count alone).
        struct Config
        {
            Arm* arm;
            std::function<void()> invalidate;
            uint64_t mask;
            uint32_t* wrong;
            std::vector<double> ms;
        };
        const auto sample = [&](Config& config)
        {
            Arm& arm = *config.arm;
            const uint32_t cells = static_cast<uint32_t>(std::popcount(config.mask));
            Rendering::FrameRequest request;
            request.demand = Rendering::FrameDemand::Roots;
            request.roots = { arm.statsId };
            config.invalidate();
            const auto before = arm.context->Evaluator().ReduceCells();
            WaitForGpuIdle();
            stopwatch.Begin();
            arm.context->RunFrame(request);
            const double gpu = stopwatch.End();
            const auto delta = CellDelta(arm.context->Evaluator().ReduceCells(), before);
            if (arm.context->Evaluator().DispatchesLastFrame() != 1) ++*config.wrong;
            else if (cells && (delta.dispatches != 1 || delta.cellsDispatched != cells ||
                               arm.context->Evaluator().ReduceCells(arm.statsId).lastMask != config.mask))
                ++*config.wrong;
            return gpu;
        };

        auto cellArm = makeArm(desktop.Texture(), false);
        auto input2 = cellArm.input.as<IEffectGraphInput2>();
        uint32_t cellsWrong = 0;
        const uint32_t counts[] = { 1, 2, 4, 8, 16, 32 };
        bool masksRight = true;
        std::vector<std::vector<RECT>> cellRects;
        std::vector<Config> configs;
        configs.reserve(std::size(counts) + 3);
        for (size_t i = 0; i < std::size(counts); ++i)
            cellRects.push_back(RectsForCells(counts[i], width, height));
        for (size_t i = 0; i < std::size(counts); ++i)
        {
            const auto& rects = cellRects[i];
            const uint64_t mask = Rendering::D3D11ComputeRunner::CellMaskForRects(
                rects.data(), static_cast<uint32_t>(rects.size()), width, height);
            masksRight = masksRight && std::popcount(mask) == static_cast<int>(counts[i]) && ((mask & 1) != 0) == (counts[i] > 1);
            configs.push_back({ &cellArm, [&rects, input2] { input2->InvalidateRects(rects.data(), static_cast<UINT32>(rects.size())); },
                                mask, &cellsWrong, {} });
        }
        const size_t wholeIndex = configs.size();
        configs.push_back({ &cellArm, [&] { cellArm.input.as<IEffectGraphInput>()->Invalidate(); }, ~uint64_t{ 0 }, &cellsWrong, {} });

        // The legacy interleaved reduction, whole.
        auto legacy = makeArm(desktop.Texture(), true);
        uint32_t legacyWrong = 0;
        const size_t legacyIndex = configs.size();
        configs.push_back({ &legacy, [&] { legacy.input.as<IEffectGraphInput>()->Invalidate(); }, 0, &legacyWrong, {} });

        // The fixed part: one cell of an 8 x 8 image, a pixel per cell.
        Dda::Desktop tiny(g_d3dDevice.get(), g_d3dContext.get(), 8, 8);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) tiny.Put(x, y, 1.0f, 1.0f, 1.0f);
        tiny.UploadAll();
        auto tinyArm = makeArm(tiny.Texture(), false);
        auto tinyInput2 = tinyArm.input.as<IEffectGraphInput2>();
        const RECT one{ 5, 5, 6, 6 };   // cell 45, not cell 0
        const size_t floorIndex = configs.size();
        configs.push_back({ &tinyArm, [&] { tinyInput2->InvalidateRects(&one, 1); }, uint64_t{ 1 } << 45, &cellsWrong, {} });

        // Round robin, the starting configuration rotating each round, so a
        // GPU clock change lands in every configuration alike and sample r of
        // each configuration comes from the same round.
        for (int rep = 0; rep < reps + 2; ++rep)
            for (size_t k = 0; k < configs.size(); ++k)
            {
                auto& config = configs[(k + static_cast<size_t>(rep)) % configs.size()];
                const double gpu = sample(config);
                if (rep >= 2) config.ms.push_back(gpu);
            }
        double byCells[std::size(counts)]{};
        for (size_t i = 0; i < std::size(counts); ++i) byCells[i] = Median(configs[i].ms);
        const double whole = Median(configs[wholeIndex].ms);
        const double legacyWhole = Median(configs[legacyIndex].ms);
        const double floor = Median(configs[floorIndex].ms);
        const bool legacyRan = legacy.context->Model().FindNode(legacy.statsId)->runtimeError.empty() &&
                               AllValues(legacy.stats.get()) .size() > 0;

        printf("  [info] %u x %u FP16, %s, median of %d interleaved rounds from an idle GPU (GPU timestamps around the "
               "frame):\n", width, height, g_useWarp ? "WARP: not a hardware number" : "hardware", reps);
        for (size_t i = 0; i < std::size(counts); ++i)
            printf("  [info]   %2u cell(s): %.3f ms\n", counts[i], byCells[i]);
        printf("  [info]   64 cells (whole): %.3f ms; interleaved v6 whole: %.3f ms (ran %d)\n", whole, legacyWhole,
               legacyRan ? 1 : 0);
        printf("  [info]   fixed part (1 cell of an 8 x 8 image): %.3f ms\n", floor);
        printf("  [info] instrument: masks right %d, %u frame(s) recomputed other than the cells asked for, %u legacy "
               "frame(s) not dispatched exactly once\n", masksRight ? 1 : 0, cellsWrong, legacyWrong);
        printf("  [info] only an integrated GPU (or WARP) was available: no discrete-GPU numbers\n");
        TEST("Stage4Cost_Instrument", masksRight && cellsWrong == 0 && legacyWrong == 0 && legacyRan);
        if (!g_useWarp)
        {
            // Paired by round; each claim fails only when the pairs contradict
            // it with 99% confidence.
            const auto& oneMs = configs[0].ms;
            const auto& eightMs = configs[3].ms;
            const auto& thirtyTwoMs = configs[5].ms;
            const auto& wholeMs = configs[wholeIndex].ms;
            PrintPairedTiming("Stage4Cost 1 cell - whole", ComparePaired(oneMs, wholeMs));
            PrintPairedTiming("Stage4Cost 32 cells - whole", ComparePaired(thirtyTwoMs, wholeMs));
            PrintPairedTiming("Stage4Cost whole - interleaved v6 whole", ComparePaired(wholeMs, configs[legacyIndex].ms));
            TIMING_TEST("Stage4Cost_OneCellBelowWhole", ComparePaired(oneMs, wholeMs).ConfidentlyBelow());
            // A real drop, not only a curve that does not rise: one cell well
            // below whole, 32 cells below whole, and no step back up.
            TIMING_TEST("Stage4Cost_SpanFallsWithCells", !ComparePaired(oneMs, wholeMs, 0.8).ConfidentlyAbove() &&
                                                         !ComparePaired(thirtyTwoMs, wholeMs, 0.9).ConfidentlyAbove() &&
                                                         !ComparePaired(oneMs, eightMs, 1.1, 0.02).ConfidentlyAbove() &&
                                                         !ComparePaired(eightMs, thirtyTwoMs, 1.1, 0.02).ConfidentlyAbove());
            TIMING_TEST("Stage4Cost_WholeNoDearerThanInterleaved",
                        !ComparePaired(wholeMs, configs[legacyIndex].ms, 1.1, 0.05).ConfidentlyAbove());
        }
    }
}

// ---- Stage 5: move rects ---------------------------------------------------
//
// The stage hands the engine each frame's moves (MoveRects) beside its dirty
// rects, and forwards what each realize reports in `moved` and `drawn`.
// Through a shift-invariant chain an incremental realize copies the moved
// pixels within its target and draws only the rects; anything else draws the
// moves' destinations. Every present is checked against a forced-whole render
// of the same frame: equal outside the pixels moved since a draw last covered
// them, and inside them within the dither of a dithering effect
// (cMoveDitherCodes; the plugin's tone map dithers by position, Gamut Map,
// the stand-in without --plugin, does not, so its moved pixels are exact).
namespace
{
    // The chain: the plugin's tone map, which declares shiftInvariant, or
    // Gamut Map with the flag set white-box (it reads only its own pixel).
    const wchar_t* MoveChainEffect() { return g_pluginPath.empty() ? L"Gamut Map" : cTonemapName; }

    Graph::EffectNode* RootNode(ReplayArm& arm)
    {
        return arm.context->Model().FindNode(arm.context->NodeId(arm.root.get()));
    }

    bool DeclaresShiftInvariant(ReplayArm& arm)
    {
        const auto* node = RootNode(arm);
        return node && node->customEffect.has_value() && node->customEffect->shiftInvariant;
    }

    void SetShiftInvariant(ReplayArm& arm, bool on)
    {
        if (auto* node = RootNode(arm); node && node->customEffect.has_value()) node->customEffect->shiftInvariant = on;
    }

    // Pixels moved since a draw last covered them: where a dithering effect's
    // moved output may differ from a fresh render.
    struct DriftMask
    {
        uint32_t width{ 0 }, height{ 0 };
        std::vector<uint8_t> mask;

        void Reset(uint32_t w, uint32_t h)
        {
            width = w;
            height = h;
            mask.assign(static_cast<size_t>(w) * h, 0);
        }
        void Set(const RECT& r, uint8_t value)
        {
            const RECT c = ClipRect(r, width, height);
            for (LONG y = c.top; y < c.bottom; ++y)
                std::memset(&mask[static_cast<size_t>(y) * width + c.left], value, static_cast<size_t>(c.right - c.left));
        }
        // A realize: its moves, then its rects; a whole draw clears it.
        void Realized(const std::vector<EFFECTGRAPH_MOVE_RECT>& moved, const std::vector<RECT>& drawn)
        {
            for (const auto& move : moved) Set(move.DestinationRect, 1);
            for (const auto& r : drawn) Set(r, 0);
        }
    };

    // An image against a forced-whole render, `drift` aside: pixels that differ
    // outside it; inside it, those that differ, those past cMoveDitherCodes,
    // and the largest code distance.
    struct DriftDiff
    {
        size_t exact{ 0 }, drifted{ 0 }, over{ 0 };
        int maxCodes{ 0 };
    };

    DriftDiff CompareWithDrift(const std::vector<uint8_t>& image, const std::vector<uint8_t>& reference,
                               const DriftMask& drift)
    {
        DriftDiff diff;
        const size_t pixels = (std::min)(image.size(), reference.size()) / 4;
        for (size_t i = 0; i < pixels; ++i)
        {
            if (std::memcmp(&image[i * 4], &reference[i * 4], 4) == 0) continue;
            if (i >= drift.mask.size() || !drift.mask[i])
            {
                ++diff.exact;
                continue;
            }
            int codes = 0;
            for (int c = 0; c < 4; ++c) codes = (std::max)(codes, std::abs(static_cast<int>(image[i * 4 + c]) - reference[i * 4 + c]));
            ++diff.drifted;
            diff.maxCodes = (std::max)(diff.maxCodes, codes);
            if (codes > Rendering::cMoveDitherCodes) ++diff.over;
        }
        return diff;
    }

    bool SameMoves(const std::vector<EFFECTGRAPH_MOVE_RECT>& a, const std::vector<EFFECTGRAPH_MOVE_RECT>& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y)
        {
            return x.SourcePoint.x == y.SourcePoint.x && x.SourcePoint.y == y.SourcePoint.y &&
                   x.DestinationRect.left == y.DestinationRect.left && x.DestinationRect.top == y.DestinationRect.top &&
                   x.DestinationRect.right == y.DestinationRect.right &&
                   x.DestinationRect.bottom == y.DestinationRect.bottom;
        });
    }

    enum class MoveChain
    {
        ShiftInvariant,    // the moves are copies: the stage under test
        NotShiftInvariant, // the root does not declare it: every move a dirty rect
        ApplyMovesOff,     // the kill switch: every move a dirty rect
    };

    struct MoveReplayResult
    {
        bool built{ false };
        bool declared{ false };   // the chain's root declares shiftInvariant
        HRESULT primed{ E_FAIL };
        uint32_t presents{ 0 }, failed{ 0 }, movePresents{ 0 };
        uint64_t handed{ 0 }, applied{ 0 }, collapsed{ 0 }, reported{ 0 }, copied{ 0 };
        // Presents whose `moved` was not the frame's moves; that drew other than
        // what they had to (only the dirty rects when every move was copied).
        uint32_t movedMisreported{ 0 }, drawnMisses{ 0 }, notAllApplied{ 0 };
        // Outside `drawn`, the target against the target before it with the
        // reported moves replayed: pixels and presents that differ.
        size_t outsidePixels{ 0 };
        uint32_t outsideFrames{ 0 };
        // Against the forced-whole render: pixels differing outside the drift
        // mask, inside it, and past cMoveDitherCodes; the largest distance.
        size_t exactPixels{ 0 }, driftPixels{ 0 }, overTolerance{ 0 };
        uint32_t exactFrames{ 0 };
        int driftMax{ 0 };
        // Clients: acquires, surfaces not equal to the stage's target, and
        // changes since the client's last frame outside the metadata it got.
        uint32_t acquires[2]{}, clientMismatches[2]{}, coverageMisses[2]{};
        // The client contract on the CPU (ClientModel): acquires where moves
        // then dirty rects did not give the target; acquires of a frame held
        // across presents that still carried moves; acquires that carried any.
        uint32_t modelMisses[2]{}, heldMoves[2]{}, movesDelivered[2]{};
        bool modelSensitive{ false };
        // Instrument: presents whose target changed nothing.
        uint32_t staticFrames{ 0 };
    };

    MoveReplayResult MoveReplay(uint32_t width, uint32_t height, const FrameSource& source, MoveChain chain)
    {
        MoveReplayResult result;
        Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        if (!source.start(desktop)) return result;
        auto incremental = MakeArm(desktop.Texture(), MoveChainEffect(), ArmKind::Incremental);
        auto control = MakeArm(desktop.Texture(), MoveChainEffect(), ArmKind::ForcedWhole);
        if (!incremental.ok || !control.ok) return result;
        if (g_pluginPath.empty()) SetShiftInvariant(incremental, true);
        if (chain == MoveChain::NotShiftInvariant) SetShiftInvariant(incremental, false);
        if (chain == MoveChain::ApplyMovesOff)
        {
            auto options = incremental.context->Evaluator().Options();
            options.applyMoves = false;
            incremental.context->Evaluator().SetOptions(options);
        }
        result.declared = DeclaresShiftInvariant(incremental);
        result.built = true;
        const bool expectCopies = chain == MoveChain::ShiftInvariant && result.declared;

        Dda::StageSim stage(incremental.context.get(), incremental.input.get(), incremental.root.get(),
                            incremental.target.get(), g_d3dContext.get());
        stage.SetMoves(true);
        winrt::com_ptr<ID3D11Texture2D> surfaces[2] = { MakeClientSurface(width, height), MakeClientSurface(width, height) };
        stage.AddClient(surfaces[0].get());   // acquires every present
        stage.AddClient(surfaces[1].get());   // holds its frame across cHoldPresents presents
        std::vector<uint8_t> seen[2];
        ClientModel models[2];
        result.primed = stage.Prime();
        if (FAILED(result.primed) || FAILED(RealizeWhole(control))) return result;
        DriftMask drift;
        drift.Reset(width, height);
        const auto acquire = [&](int client)
        {
            const auto delivery = stage.Acquire(client);
            if (FAILED(delivery.hr)) return;
            ++result.acquires[client];
            const auto surface = Dda::ReadBytes(surfaces[client].get());
            const auto target = Dda::ReadBytes(incremental.target.get());
            if (Dda::Compare(surface, target, width, {}).differing) ++result.clientMismatches[client];
            if (!seen[client].empty() && Dda::Compare(seen[client], target, width, delivery.rects).outside)
                ++result.coverageMisses[client];
            // A client applying the moves to its own copy, then the dirty
            // rects: it must end at the target. A held frame gets rects only.
            if (seen[client].empty()) models[client].Start(target, width, height);
            else
            {
                // Instrument: the same delivery without its moves must not
                // reach the target, or the check proves nothing about them.
                if (!delivery.moves.empty() && !result.modelSensitive)
                {
                    auto probe = models[client];
                    auto withoutMoves = delivery;
                    withoutMoves.moves.clear();
                    result.modelSensitive = !probe.Apply(withoutMoves, target);
                }
                if (!models[client].Apply(delivery, target)) ++result.modelMisses[client];
            }
            if (delivery.presents > 1 && !delivery.moves.empty()) ++result.heldMoves[client];
            if (!delivery.moves.empty()) ++result.movesDelivered[client];
            seen[client] = surface;
        };
        acquire(0);
        acquire(1);
        const auto statsBefore = incremental.context->Evaluator().RegionStats();

        Dda::DesktopFrame frame;
        while (source.next(desktop, frame))
        {
            if (frame.kind == Dda::FrameKind::NoFrame) continue;
            ++result.presents;
            if (!frame.moves.empty()) ++result.movePresents;
            result.handed += frame.moves.size();
            auto before = Dda::ReadBytes(incremental.target.get());
            const auto stats0 = incremental.context->Evaluator().RegionStats();
            const HRESULT hr = stage.Present(frame);
            const HRESULT hrControl = RealizeWhole(control);
            if (FAILED(hr) || FAILED(hrControl))
            {
                ++result.failed;
                continue;
            }
            const auto& stats1 = incremental.context->Evaluator().RegionStats();
            const uint64_t applied = stats1.movesApplied - stats0.movesApplied;
            const bool wholeDraw = stats1.wholeDraws > stats0.wholeDraws;
            if (!SameMoves(stage.LastMoved(), frame.moves)) ++result.movedMisreported;
            if (expectCopies && !wholeDraw && applied != frame.moves.size()) ++result.notAllApplied;
            // Only the dirty rects when the moves were copied; their
            // destinations too when they were not.
            const auto expected = expectCopies ? frame.dirty : Dda::ChangedRects(frame);
            if (!DrawnAsExpected(expected, frame.whole, stage.LastDrawn(), width, height)) ++result.drawnMisses;

            const auto after = Dda::ReadBytes(incremental.target.get());
            if (Dda::Compare(before, after, width, {}).differing == 0) ++result.staticFrames;
            if (!wholeDraw) MoveBytes(before, width, stage.LastMoved());
            const auto outside = Dda::Compare(before, after, width, stage.LastDrawn()).outside;
            if (outside && !wholeDraw)
            {
                ++result.outsideFrames;
                result.outsidePixels += outside;
            }
            // Every draw that is not whole: the moves mark their destinations,
            // then the rects drawn clear theirs, a collapsed move's included.
            if (wholeDraw) drift.Reset(width, height);
            else drift.Realized(stage.LastMoved(), stage.LastDrawn());
            const auto diff = CompareWithDrift(after, Dda::ReadBytes(control.target.get()), drift);
            if (diff.exact) ++result.exactFrames;
            result.exactPixels += diff.exact;
            result.driftPixels += diff.drifted;
            result.overTolerance += diff.over;
            result.driftMax = (std::max)(result.driftMax, diff.maxCodes);

            acquire(0);
            if (result.presents % cHoldPresents == 0) acquire(1);
        }
        const auto& statsAfter = incremental.context->Evaluator().RegionStats();
        result.applied = statsAfter.movesApplied - statsBefore.movesApplied;
        result.collapsed = statsAfter.movesCollapsed - statsBefore.movesCollapsed;
        result.reported = statsAfter.movesReported - statsBefore.movesReported;
        result.copied = statsAfter.movePixelsCopied - statsBefore.movePixelsCopied;
        return result;
    }

    void ReportMoveReplay(const std::string& name, const MoveReplayResult& r, MoveChain chain)
    {
        const auto per100 = [&](double n) { return r.presents ? 100.0 * n / r.presents : 0.0; };
        printf("  [info] %s: %u presents (%u with moves), %u failed; moves handed %llu, applied %llu (%.1f per 100 "
               "presents), collapsed %llu (%.1f per 100), reported %llu; %llu px copied\n",
               name.c_str(), r.presents, r.movePresents, r.failed, static_cast<unsigned long long>(r.handed),
               static_cast<unsigned long long>(r.applied), per100(static_cast<double>(r.applied)),
               static_cast<unsigned long long>(r.collapsed), per100(static_cast<double>(r.collapsed)),
               static_cast<unsigned long long>(r.reported), static_cast<unsigned long long>(r.copied));
        printf("  [info] %s: against the forced-whole render, %zu px differ outside moved pixels; inside them %zu px "
               "differ, by up to %d code(s), %zu past %d; %zu px changed outside drawn and the moves\n", name.c_str(),
               r.exactPixels, r.driftPixels, r.driftMax, r.overTolerance, Rendering::cMoveDitherCodes, r.outsidePixels);
        TEST((name + "_Built").c_str(), r.built && SUCCEEDED(r.primed) && r.presents > 0 && r.failed == 0);
        TEST((name + "_LivePipeline").c_str(), r.presents > 0 && r.staticFrames == 0);
        // Every move handed is reported, in order, whether copied or drawn.
        TEST((name + "_MovedReportsEveryMove").c_str(), r.movedMisreported == 0 && r.reported == r.handed);
        TEST((name + "_NothingOutsideDrawnAndMoves").c_str(), r.outsideFrames == 0);
        TEST((name + "_DrawsWhatItMust").c_str(), r.drawnMisses == 0);
        TEST((name + "_ClientsCurrent").c_str(),
             r.acquires[0] == r.presents + 1 && r.clientMismatches[0] == 0 && r.coverageMisses[0] == 0 &&
             r.acquires[1] >= 2 && r.clientMismatches[1] == 0 && r.coverageMisses[1] == 0);
        // Moves forwarded to a client that applies them to its own copy.
        TEST((name + "_ClientModelCurrent").c_str(), r.modelMisses[0] == 0 && r.modelMisses[1] == 0);
        TEST((name + "_HeldFrameGetsRectsOnly").c_str(), r.heldMoves[1] == 0);
        if (chain == MoveChain::ShiftInvariant && r.declared && r.handed > 0)
            TEST((name + "_ClientGotMoves").c_str(), r.movesDelivered[0] > 0 && r.modelSensitive);
        if (chain == MoveChain::ShiftInvariant)
        {
            // Copied unless the present drew whole (a whole draw copies nothing).
            TEST((name + "_EveryMoveCopied").c_str(), r.notAllApplied == 0 && r.applied + r.collapsed == r.handed &&
                                                     (r.handed == 0 || r.applied > 0));
            // Only dither inside moved pixels, nothing anywhere else.
            TEST((name + "_EqualsForcedWholeOutsideMoves").c_str(), r.exactFrames == 0);
            TEST((name + "_MovedWithinDither").c_str(), r.overTolerance == 0);
            if (g_pluginPath.empty())
                TEST((name + "_MovedExactWithoutDither").c_str(), r.driftPixels == 0);
        }
        else
        {
            // A move drawn as a dirty rect: the target is the forced-whole render.
            TEST((name + "_EveryMoveDrawn").c_str(), r.applied == 0 && r.collapsed == r.handed && r.copied == 0);
            TEST((name + "_EqualsForcedWhole").c_str(), r.exactFrames == 0 && r.driftPixels == 0);
        }
    }

    SL_TEST_CASE(Stage5_ReplaySynthetic, Tag::Gpu)
    {
        printf("\n=== Stage 5: replay with moves (%ls) ===\n", MoveChainEffect());
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        bool declared = true;
        uint64_t applied = 0;
        for (const auto kind : cScenarios)
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = MoveReplay(width, height, SyntheticSource(*scenario), MoveChain::ShiftInvariant);
            declared = declared && result.declared;
            applied += result.applied;
            ReportMoveReplay(std::string("Moves_") + Dda::ScenarioName(kind), result, MoveChain::ShiftInvariant);
        }
        // The plugin's own JSON declares it (PRIV effects/hdr_screenshot_tonemap.json).
        // Without the plugin the stand-in's flag is set white-box: nothing to check.
        if (g_pluginPath.empty())
            ReportNotRun("Moves_ChainDeclaresShiftInvariant: needs --plugin <PrivateEffects.dll>");
        else
            TEST("Moves_ChainDeclaresShiftInvariant", declared);
        // The scrolls and the drag carry moves: the copy path ran.
        TEST("Moves_SomeMovesCopied", applied > 0);
        for (const auto kind : { Dda::ScenarioKind::Scroll, Dda::ScenarioKind::WindowDrag, Dda::ScenarioKind::VideoAndScroll })
        {
            auto off = Dda::MakeScenario(kind, presents);
            ReportMoveReplay(std::string("MovesOff_") + Dda::ScenarioName(kind),
                             MoveReplay(width, height, SyntheticSource(*off), MoveChain::ApplyMovesOff), MoveChain::ApplyMovesOff);
            auto plain = Dda::MakeScenario(kind, presents);
            ReportMoveReplay(std::string("MovesNotShiftInvariant_") + Dda::ScenarioName(kind),
                             MoveReplay(width, height, SyntheticSource(*plain), MoveChain::NotShiftInvariant),
                             MoveChain::NotShiftInvariant);
        }
    }

    // ---- Targeted cases on a small desktop: a shift-invariant root (the
    // given effect with the flag set white-box), its forced-whole control, and
    // realizes that report `moved`.
    struct MoveRig
    {
        std::unique_ptr<Dda::Desktop> desktop;
        ReplayArm incremental, control;
        winrt::com_ptr<IEffectGraphInput2> input2;
        uint32_t width{ 0 }, height{ 0 };
        std::vector<RECT> drawn;
        std::vector<EFFECTGRAPH_MOVE_RECT> moved;
        bool whole{ false };
        bool ok{ false };

        Rendering::RegionDrawStats Stats() { return incremental.context->Evaluator().RegionStats(); }

        HRESULT Realize(UINT32 movedCapacity = 16)
        {
            RECT rects[16]{};
            EFFECTGRAPH_MOVE_RECT moves[16]{};
            UINT32 count = 0, movedCount = 0;
            const HRESULT hr = incremental.context.as<IEffectGraphContext2>()->RealizeToIncremental(
                incremental.root.get(), incremental.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, rects, 16, &count,
                movedCapacity ? moves : nullptr, movedCapacity, movedCapacity ? &movedCount : nullptr);
            drawn.assign(rects, rects + (SUCCEEDED(hr) ? count : 0));
            moved.assign(moves, moves + (SUCCEEDED(hr) ? movedCount : 0));
            whole = drawn.size() == 1 && drawn[0].left == 0 && drawn[0].top == 0 &&
                    drawn[0].right == static_cast<LONG>(width) && drawn[0].bottom == static_cast<LONG>(height);
            return hr;
        }
        // The desktop moves the pixels and the engine is told.
        void Move(const EFFECTGRAPH_MOVE_RECT& move)
        {
            desktop->ApplyMoves({ move });
            desktop->Upload({ move.DestinationRect });
            input2->MoveRects(&move, 1);
        }
        // A rect painted a flat colour and reported.
        void Paint(const RECT& r, float grey)
        {
            for (LONG y = r.top; y < r.bottom; ++y)
                for (LONG x = r.left; x < r.right; ++x)
                    desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), grey, grey * 0.5f, 0.2f);
            desktop->Upload({ r });
            input2->InvalidateRects(&r, 1);
        }
        // The target against a forced-whole render of the desktop now.
        size_t DiffersFromWhole()
        {
            if (FAILED(RealizeWhole(control))) return SIZE_MAX;
            return Dda::Compare(Dda::ReadBytes(incremental.target.get()), Dda::ReadBytes(control.target.get()), width, {})
                .differing;
        }
        bool Drew(const RECT& r) const
        {
            for (LONG y = r.top; y < r.bottom; ++y)
                for (LONG x = r.left; x < r.right; ++x)
                    if (!Dda::Inside(drawn, x, y)) return false;
            return true;
        }
    };

    // A test-only stand-in for a dithering shift-invariant root: the input
    // encoded with the sRGB curve, quantized to 8 bits with the dither the
    // plugin's tone map uses (DitherQuantize, by floor(scenePos)), and decoded.
    Effects::ShaderLabEffectDescriptor DitherRootDescriptor()
    {
        Effects::ShaderLabEffectDescriptor d;
        d.name = L"Stage5 Dither Root";
        d.effectId = L"Stage5 Dither Root";
        d.effectVersion = 1;
        d.shaderType = Graph::CustomShaderType::PixelShader;
        d.inputNames = { L"Source" };
        d.shiftInvariant = true;   // reads only its own pixel; dithers by position
        d.hlslSource = R"HLSL(
#include "shaderlab_colormath.hlsli"
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    float DitherStrength;
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float strength = DitherStrength;
    float4 c = Source.Sample(InputSampler, uv0.xy);
    float3 encoded = DitherQuantize(LinearToSRGB(saturate(c.rgb)), scenePos.xy, 256.0, strength);
    return float4(SRGBToLinear(encoded), c.a);
}
)HLSL";
        d.parameters = {
            Graph::ParameterDefinition{ L"DitherStrength", L"float", 1.0f, 0.0f, 2.0f, 0.05f },
        };
        return d;
    }

    // The arm's root replaced by the dithering stand-in at `strength`.
    bool UseDitherRoot(ReplayArm& arm, float strength)
    {
        static const auto descriptor = DitherRootDescriptor();
        if (arm.root) arm.context->RemoveNode(arm.root.get());
        arm.root = nullptr;
        const uint32_t id = arm.context->Model().AddNode(Effects::ShaderLabEffects::CreateNode(descriptor));
        return arm.context->NodeFor(id, arm.root.put()) == S_OK && SUCCEEDED(arm.root->SetInput(0, arm.input.get())) &&
               SUCCEEDED(arm.root->SetValueByName(L"DitherStrength", strength));
    }

    // `ditherStrength` 0 or more: both roots are the dithering stand-in.
    MoveRig MakeMoveRig(const wchar_t* effect, uint32_t width = 160, uint32_t height = 96, float ditherStrength = -1.0f)
    {
        MoveRig rig;
        rig.width = width;
        rig.height = height;
        rig.desktop = std::make_unique<Dda::Desktop>(g_d3dDevice.get(), g_d3dContext.get(), width, height);
        // A gradient with structure, so a wrong copy shows.
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                rig.desktop->Put(x, y, 0.05f + 3.0f * x / width, 0.05f + 0.9f * y / height,
                                 ((x / 7 + y / 5) % 3) * 0.4f);
        rig.desktop->UploadAll();
        rig.incremental = MakeArm(rig.desktop->Texture(), effect, ArmKind::Incremental);
        rig.control = MakeArm(rig.desktop->Texture(), effect, ArmKind::ForcedWhole);
        if (!rig.incremental.ok || !rig.control.ok) return rig;
        if (ditherStrength >= 0.0f &&
            (!UseDitherRoot(rig.incremental, ditherStrength) || !UseDitherRoot(rig.control, ditherStrength)))
            return rig;
        SetShiftInvariant(rig.incremental, true);
        rig.input2 = rig.incremental.input.as<IEffectGraphInput2>();
        rig.ok = SUCCEEDED(rig.Realize()) && SUCCEEDED(RealizeWhole(rig.control));
        return rig;
    }

    SL_TEST_CASE(Stage5_MoveCases, Tag::Gpu)
    {
        printf("\n=== Stage 5: moves, case by case ===\n");
        // The revealed strip of a scroll lies inside the move's source; it is
        // the same frame's, so the move stays a copy and only the strip is drawn.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            TEST("MoveCase_Built", rig.ok);
            if (!rig.ok) return;
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT scroll{ { 10, 20 }, { 10, 10, 150, 86 } };
            rig.Move(scroll);
            const RECT strip{ 10, 86, 150, 96 };
            rig.Paint(strip, 1.5f);
            const uint64_t copiesBefore = Api::Testing::g_moveCopies;
            const uint64_t inPlaceBefore = Api::Testing::g_moveCopiesInPlace;
            const HRESULT hr = rig.Realize();
            const uint64_t copies = Api::Testing::g_moveCopies - copiesBefore;
            const uint64_t inPlace = Api::Testing::g_moveCopiesInPlace - inPlaceBefore;
            const auto after = rig.Stats();
            const size_t differs = rig.DiffersFromWhole();
            printf("  [info] scroll: %zu rect(s) drawn, %llu move(s) copied, %zu px differ from the forced-whole render\n",
                   rig.drawn.size(), static_cast<unsigned long long>(after.movesApplied - before.movesApplied), differs);
            TEST("MoveCase_ScrollStaysAMove", hr == S_OK && after.movesApplied - before.movesApplied == 1 &&
                                              after.movesCollapsed == before.movesCollapsed);
            TEST("MoveCase_ScrollDrawsOnlyTheStrip", rig.drawn.size() == 1 && rig.drawn[0].left == strip.left &&
                                                     rig.drawn[0].top == strip.top && rig.drawn[0].right == strip.right &&
                                                     rig.drawn[0].bottom == strip.bottom);
            TEST("MoveCase_ScrollReported", SameMoves(rig.moved, { scroll }));
            TEST("MoveCase_ScrollEqualsForcedWhole", differs == 0);
            // Two CopySubresourceRegion calls for the one move, neither within
            // one resource (the overlapping in-place copy D3D11 leaves undefined).
            TEST("MoveCase_CopiedTwiceThroughScratch", copies == 2 && inPlace == 0 &&
                 after.movePixelsCopied - before.movePixelsCopied == 140ull * 76ull);
        }
        // A second MoveRects before a realize: the first batch is drawn at its
        // destination; the second is copied; both are reported, in order.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT first{ { 0, 0 }, { 20, 0, 60, 30 } };
            const EFFECTGRAPH_MOVE_RECT second{ { 100, 50 }, { 110, 60, 150, 90 } };
            rig.Move(first);
            rig.Move(second);
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveCase_SecondBatchCollapsesTheFirst", rig.ok && hr == S_OK &&
                 after.movesCollapsed - before.movesCollapsed == 1 && after.movesApplied - before.movesApplied == 1 &&
                 rig.Drew(first.DestinationRect) && !rig.Drew(second.DestinationRect));
            TEST("MoveCase_SecondBatchReportsBoth", SameMoves(rig.moved, { first, second }));
            TEST("MoveCase_SecondBatchEqualsForcedWhole", rig.DiffersFromWhole() == 0);
        }
        // Chained moves: M2 copies from M1's destination. Both copied, in order.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT m1{ { 0, 0 }, { 40, 0, 80, 40 } };
            const EFFECTGRAPH_MOVE_RECT m2{ { 40, 0 }, { 100, 40, 140, 80 } };
            rig.desktop->ApplyMoves({ m1, m2 });
            rig.desktop->Upload({ m1.DestinationRect, m2.DestinationRect });
            const EFFECTGRAPH_MOVE_RECT both[] = { m1, m2 };
            rig.input2->MoveRects(both, 2);
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveCase_ChainedMovesCopiedInOrder", rig.ok && hr == S_OK && after.movesApplied - before.movesApplied == 2 &&
                                                       rig.drawn.empty() && rig.DiffersFromWhole() == 0);
        }
        // Collapsed M1, then M2 whose source overlaps M1's destination: M1's
        // source reads rects a failed realize left (older than the move), so it
        // is drawn; M2 would copy M1's un-updated pixels, so it is drawn too.
        {
            auto rig = MakeMoveRig(L"ICtCp Gamut Map");
            TEST("MoveCase_FaultyBuilt", rig.ok);
            if (!rig.ok) return;
            FaultyHooks hooks;
            rig.incremental.context->Evaluator().SetEffectHooks(&hooks);
            struct RestoreHooks
            {
                IEffectGraphContextInternal* context;
                ~RestoreHooks() { context->Evaluator().SetEffectHooks(&Effects::BuiltInEffectHooks()); }
            } restore{ rig.incremental.context.get() };

            hooks.failing = true;
            const RECT stale{ 0, 0, 30, 30 };
            rig.Paint(stale, 2.0f);
            const HRESULT failed = rig.Realize();
            hooks.failing = false;
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT m1{ { 10, 10 }, { 60, 10, 90, 40 } };   // source inside `stale`
            const EFFECTGRAPH_MOVE_RECT m2{ { 70, 20 }, { 110, 50, 140, 80 } };  // source inside m1's destination
            rig.desktop->ApplyMoves({ m1, m2 });
            rig.desktop->Upload({ m1.DestinationRect, m2.DestinationRect });
            const EFFECTGRAPH_MOVE_RECT both[] = { m1, m2 };
            rig.input2->MoveRects(both, 2);
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            printf("  [info] collapse chain: realize %s then %s, %llu collapsed, %llu copied\n",
                   failed == EFFECTGRAPH_E_NODE_FAILED ? "failed" : "did not fail", hr == S_OK ? "S_OK" : "an error",
                   static_cast<unsigned long long>(after.movesCollapsed - before.movesCollapsed),
                   static_cast<unsigned long long>(after.movesApplied - before.movesApplied));
            // Incremental: a whole draw would satisfy the rest without the rule.
            TEST("MoveCase_CollapsedChainBothDrawn", failed == EFFECTGRAPH_E_NODE_FAILED && hr == S_OK &&
                 after.wholeDraws == before.wholeDraws && !rig.whole &&
                 after.movesCollapsed - before.movesCollapsed == 2 && after.movesApplied == before.movesApplied &&
                 rig.Drew(stale) && rig.Drew(m1.DestinationRect) && rig.Drew(m2.DestinationRect));
            TEST("MoveCase_CollapsedChainReported", SameMoves(rig.moved, { m1, m2 }));
            TEST("MoveCase_CollapsedChainEqualsForcedWhole", rig.DiffersFromWhole() == 0);

            // The root errors on N with rects, a move on N+1 whose source
            // overlaps them (the root still errored), recovery on N+2: the
            // move is a carry-over, drawn at its destination, and reported.
            hooks.failing = true;
            const RECT rects{ 100, 0, 140, 30 };
            rig.Paint(rects, 0.7f);
            const HRESULT n = rig.Realize();
            const EFFECTGRAPH_MOVE_RECT move{ { 105, 5 }, { 20, 50, 50, 75 } };
            rig.Move(move);
            const HRESULT n1 = rig.Realize();
            hooks.failing = false;
            const auto beforeRecovery = rig.Stats();
            const HRESULT n2 = rig.Realize();
            const auto afterRecovery = rig.Stats();
            TEST("MoveCase_ErroredThenMoveFails", n == EFFECTGRAPH_E_NODE_FAILED && n1 == EFFECTGRAPH_E_NODE_FAILED);
            TEST("MoveCase_RecoveryDrawsTheDestination", n2 == S_OK && rig.Drew(rects) && rig.Drew(move.DestinationRect) &&
                 afterRecovery.wholeDraws == beforeRecovery.wholeDraws && !rig.whole &&
                 afterRecovery.movesCollapsed - beforeRecovery.movesCollapsed == 1 &&
                 afterRecovery.movesApplied == beforeRecovery.movesApplied);
            TEST("MoveCase_RecoveryReportsTheMove", SameMoves(rig.moved, { move }));
            TEST("MoveCase_RecoveryEqualsForcedWhole", rig.DiffersFromWhole() == 0);

            // A failed realize leaves a stale rect; a move whose source misses
            // it is no carry-over (it came after the failure) and is copied.
            hooks.failing = true;
            const RECT staleAgain{ 0, 0, 30, 30 };
            rig.Paint(staleAgain, 1.1f);
            const HRESULT failedAgain = rig.Realize();
            hooks.failing = false;
            const auto beforeApart = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT apart{ { 100, 50 }, { 110, 10, 150, 40 } };
            rig.Move(apart);
            const HRESULT apartHr = rig.Realize();
            const auto afterApart = rig.Stats();
            TEST("MoveCase_StaleRectLeavesAnUnrelatedMove", failedAgain == EFFECTGRAPH_E_NODE_FAILED && apartHr == S_OK &&
                 afterApart.wholeDraws == beforeApart.wholeDraws && !rig.whole &&
                 afterApart.movesApplied - beforeApart.movesApplied == 1 &&
                 afterApart.movesCollapsed == beforeApart.movesCollapsed &&
                 rig.Drew(staleAgain) && !rig.Drew(apart.DestinationRect) && SameMoves(rig.moved, { apart }) &&
                 rig.DiffersFromWhole() == 0);
        }
        // More moves than `moved` holds: none can be forwarded, so none is
        // copied; every destination is drawn.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT a{ { 0, 0 }, { 20, 0, 50, 30 } };
            const EFFECTGRAPH_MOVE_RECT b{ { 80, 40 }, { 100, 60, 130, 90 } };
            const EFFECTGRAPH_MOVE_RECT both[] = { a, b };
            rig.desktop->ApplyMoves({ a, b });
            rig.desktop->Upload({ a.DestinationRect, b.DestinationRect });
            rig.input2->MoveRects(both, 2);
            const HRESULT hr = rig.Realize(1);
            const auto after = rig.Stats();
            TEST("MoveCase_OverCapacityDrawsAll", rig.ok && hr == S_OK && after.movesApplied == before.movesApplied &&
                 after.movesCollapsed - before.movesCollapsed == 2 && rig.moved.empty() &&
                 rig.Drew(a.DestinationRect) && rig.Drew(b.DestinationRect) && rig.DiffersFromWhole() == 0);
        }
        // Clipping, in one batch: a destination off the right edge keeps the
        // part inside; one off the left and top keeps the part inside with
        // its source shifted by as much; one with nothing left is dropped.
        // The clipped moves are copied, so the clip is what is under test.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT batch[] = {
                { { 0, 0 }, { 140, 0, 180, 20 } },      // 160 wide
                { { 60, 50 }, { -20, -10, 20, 30 } },
                { { 0, 0 }, { 200, 0, 240, 20 } },
            };
            const EFFECTGRAPH_MOVE_RECT offRight{ { 0, 0 }, { 140, 0, 160, 20 } };
            const EFFECTGRAPH_MOVE_RECT offLeftTop{ { 80, 60 }, { 0, 0, 20, 30 } };
            const HRESULT clippedHr = rig.input2->MoveRects(batch, 3);
            rig.desktop->ApplyMoves({ offRight, offLeftTop });
            rig.desktop->Upload({ offRight.DestinationRect, offLeftTop.DestinationRect });
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveCase_Clipped", clippedHr == S_OK && hr == S_OK && !rig.whole &&
                 after.movesApplied - before.movesApplied == 2 && after.movesCollapsed == before.movesCollapsed &&
                 rig.drawn.empty() && SameMoves(rig.moved, { offRight, offLeftTop }) && rig.DiffersFromWhole() == 0);
            // Nothing left after clipping: S_FALSE, and the batch before it
            // stays a move (the call changed nothing).
            const auto beforeGone = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT kept{ { 30, 40 }, { 50, 50, 90, 80 } };
            rig.Move(kept);
            const HRESULT goneHr = rig.input2->MoveRects(&batch[2], 1);
            const HRESULT keptHr = rig.Realize();
            const auto afterGone = rig.Stats();
            TEST("MoveCase_ClippedAwayChangesNothing", goneHr == S_FALSE && keptHr == S_OK &&
                 afterGone.movesApplied - beforeGone.movesApplied == 1 &&
                 afterGone.movesCollapsed == beforeGone.movesCollapsed && SameMoves(rig.moved, { kept }) &&
                 rig.DiffersFromWhole() == 0);
            // A source off the texture: no move. The whole destination is
            // drawn and the move is not reported.
            const auto beforeSource = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT sourceOff{ { -20, 0 }, { 0, 60, 40, 80 } };
            for (LONG y = 60; y < 80; ++y)
                for (LONG x = 0; x < 40; ++x)
                    rig.desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 2.5f, 0.3f, 0.6f);
            rig.desktop->Upload({ sourceOff.DestinationRect });
            const HRESULT sourceOffHr = rig.input2->MoveRects(&sourceOff, 1);
            const HRESULT drawnHr = rig.Realize();
            const auto afterSource = rig.Stats();
            TEST("MoveCase_SourceOffTextureDrawn", sourceOffHr == S_OK && drawnHr == S_OK && !rig.whole &&
                 rig.Drew(sourceOff.DestinationRect) && rig.moved.empty() &&
                 afterSource.movesApplied == beforeSource.movesApplied && rig.DiffersFromWhole() == 0);
        }
        // verifyDirtyRegions replays the moves it applied: a true move passes
        // both checks; a move the host lied about (the input at the destination
        // is not the source's pixels) fails the whole-render check.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            auto options = rig.incremental.context->Evaluator().Options();
            options.verifyDirtyRegions = true;
            rig.incremental.context->Evaluator().SetOptions(options);
            const auto before = rig.Stats();
            rig.Move({ { 0, 0 }, { 60, 30, 100, 70 } });
            const HRESULT honest = rig.Realize();
            const auto mid = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT lie{ { 0, 0 }, { 110, 10, 150, 50 } };
            for (LONG y = lie.DestinationRect.top; y < lie.DestinationRect.bottom; ++y)
                for (LONG x = lie.DestinationRect.left; x < lie.DestinationRect.right; ++x)
                    rig.desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 4.0f, 0.1f, 0.1f);
            rig.desktop->Upload({ lie.DestinationRect });
            rig.input2->MoveRects(&lie, 1);
            const HRESULT lied = rig.Realize();
            const auto after = rig.Stats();
            printf("  [info] verify: honest move %llu outside / %llu whole mismatches; lying move %llu / %llu\n",
                   static_cast<unsigned long long>(mid.verifyOutsideMismatches - before.verifyOutsideMismatches),
                   static_cast<unsigned long long>(mid.verifyWholeMismatches - before.verifyWholeMismatches),
                   static_cast<unsigned long long>(after.verifyOutsideMismatches - mid.verifyOutsideMismatches),
                   static_cast<unsigned long long>(after.verifyWholeMismatches - mid.verifyWholeMismatches));
            TEST("MoveCase_VerifyPassesAMove", honest == S_OK && mid.verified > before.verified &&
                 mid.movesApplied > before.movesApplied && mid.verifyOutsideMismatches == before.verifyOutsideMismatches &&
                 mid.verifyWholeMismatches == before.verifyWholeMismatches);
            TEST("MoveCase_VerifyCatchesALie", lied == S_OK && after.verifyOutsideMismatches == mid.verifyOutsideMismatches &&
                 after.verifyWholeMismatches > mid.verifyWholeMismatches);
        }
        // The same over a root that dithers by position, at the default strength
        // and at the tone map's maximum: an honest move drifts (the tolerance is
        // what lets it pass), a later rect over part of it is exact again, and a
        // lie inside the moved pixels is still a mismatch.
        for (const float strength : { 1.0f, 2.0f })
        {
            const std::string name = "MoveCase_VerifyDither" + std::to_string(static_cast<int>(strength));
            auto rig = MakeMoveRig(L"Gamut Map", 160, 96, strength);
            TEST((name + "_Built").c_str(), rig.ok);
            if (!rig.ok) continue;
            auto options = rig.incremental.context->Evaluator().Options();
            options.verifyDirtyRegions = true;
            rig.incremental.context->Evaluator().SetOptions(options);
            const auto before = rig.Stats();
            // Moved by an odd offset on both axes, so every pixel gets another
            // position's dither.
            const EFFECTGRAPH_MOVE_RECT honest{ { 3, 5 }, { 40, 20, 120, 76 } };
            rig.Move(honest);
            const HRESULT honestHr = rig.Realize();
            const auto mid = rig.Stats();
            // Its drift against a fresh render, measured off the engine's path.
            const auto target = Dda::ReadBytes(rig.incremental.target.get());
            if (FAILED(RealizeWhole(rig.control))) continue;
            const auto fresh = Dda::ReadBytes(rig.control.target.get());
            int maxCodes = 0;
            size_t drifted = 0;
            for (size_t i = 0; i + 3 < target.size(); i += 4)
            {
                int codes = 0;
                for (int c = 0; c < 4; ++c) codes = (std::max)(codes, std::abs(static_cast<int>(target[i + c]) - fresh[i + c]));
                if (codes) ++drifted;
                maxCodes = (std::max)(maxCodes, codes);
            }
            // A rect redraws part of the moved pixels; then a lie inside them.
            const RECT redraw{ 40, 20, 80, 40 };
            rig.Paint(redraw, 0.9f);
            const HRESULT redrawHr = rig.Realize();
            const auto afterRedraw = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT lie{ { 100, 60 }, { 50, 45, 90, 70 } };
            for (LONG y = lie.DestinationRect.top; y < lie.DestinationRect.bottom; ++y)
                for (LONG x = lie.DestinationRect.left; x < lie.DestinationRect.right; ++x)
                    rig.desktop->Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0.02f, 0.9f, 0.6f);
            rig.desktop->Upload({ lie.DestinationRect });
            rig.input2->MoveRects(&lie, 1);
            const HRESULT liedHr = rig.Realize();
            const auto after = rig.Stats();
            printf("  [info] verify, dithering root at strength %.0f: honest move %llu px drift, %llu whole mismatches "
                   "(%zu px off a fresh render, by up to %d codes); after a rect %llu / %llu; lying move %llu whole "
                   "mismatches\n", strength,
                   static_cast<unsigned long long>(mid.verifyMoveDrift - before.verifyMoveDrift),
                   static_cast<unsigned long long>(mid.verifyWholeMismatches - before.verifyWholeMismatches), drifted,
                   maxCodes, static_cast<unsigned long long>(afterRedraw.verifyMoveDrift - mid.verifyMoveDrift),
                   static_cast<unsigned long long>(afterRedraw.verifyWholeMismatches - mid.verifyWholeMismatches),
                   static_cast<unsigned long long>(after.verifyWholeMismatches - afterRedraw.verifyWholeMismatches));
            // The instrument: the copy really drifted, within the tolerance.
            TEST((name + "_MoveDrifts").c_str(), honestHr == S_OK && mid.movesApplied - before.movesApplied == 1 &&
                 drifted > 0 && maxCodes <= Rendering::cMoveDitherCodes);
            TEST((name + "_HonestMovePasses").c_str(), mid.verified > before.verified &&
                 mid.verifyMoveDrift - before.verifyMoveDrift == drifted &&
                 mid.verifyOutsideMismatches == before.verifyOutsideMismatches &&
                 mid.verifyWholeMismatches == before.verifyWholeMismatches);
            // Redrawn pixels leave the mask: fewer drift, none mismatch.
            TEST((name + "_RedrawClearsTheMask").c_str(), redrawHr == S_OK && afterRedraw.verified > mid.verified &&
                 afterRedraw.verifyMoveDrift - mid.verifyMoveDrift < drifted &&
                 afterRedraw.verifyWholeMismatches == mid.verifyWholeMismatches &&
                 afterRedraw.verifyOutsideMismatches == mid.verifyOutsideMismatches);
            TEST((name + "_LieCaught").c_str(), liedHr == S_OK && after.movesApplied - afterRedraw.movesApplied == 1 &&
                 after.verifyWholeMismatches - afterRedraw.verifyWholeMismatches >= 40ull * 25ull / 2);
        }
    }

    // A further shift-invariant root on an arm's context, reading its input,
    // with its own target.
    struct ExtraRoot
    {
        winrt::com_ptr<IEffectGraphContext2> context2;
        winrt::com_ptr<IEffectGraphNode> root;
        winrt::com_ptr<ID3D11Texture2D> target;
        std::vector<RECT> drawn;
        std::vector<EFFECTGRAPH_MOVE_RECT> moved;
        uint32_t width{ 0 }, height{ 0 };
        bool ok{ false };

        HRESULT Realize()
        {
            RECT rects[16]{};
            EFFECTGRAPH_MOVE_RECT moves[16]{};
            UINT32 count = 0, movedCount = 0;
            const HRESULT hr = context2->RealizeToIncremental(root.get(), target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL,
                                                              rects, 16, &count, moves, 16, &movedCount);
            drawn.assign(rects, rects + (SUCCEEDED(hr) ? count : 0));
            moved.assign(moves, moves + (SUCCEEDED(hr) ? movedCount : 0));
            return hr;
        }
        bool Whole() const
        {
            return drawn.size() == 1 && drawn[0].left == 0 && drawn[0].top == 0 &&
                   drawn[0].right == static_cast<LONG>(width) && drawn[0].bottom == static_cast<LONG>(height);
        }
    };

    ExtraRoot AddRoot(ReplayArm& arm, const wchar_t* effect, uint32_t width, uint32_t height)
    {
        ExtraRoot extra;
        extra.width = width;
        extra.height = height;
        extra.context2 = arm.context.as<IEffectGraphContext2>();
        extra.ok = SUCCEEDED(arm.context->CreateEffect(effect, extra.root.put())) &&
                   SUCCEEDED(extra.root->SetInput(0, arm.input.get()));
        if (extra.ok)
            if (auto* node = arm.context->Model().FindNode(arm.context->NodeId(extra.root.get()));
                node && node->customEffect.has_value())
                node->customEffect->shiftInvariant = true;
        extra.target = MakeSrgbTarget(width, height);
        return extra;
    }

    // The extra root's forced-whole render on the control arm's context,
    // against its incremental target: pixels that differ.
    size_t ExtraDiffersFromWhole(MoveRig& rig, ExtraRoot& extra, ExtraRoot& control)
    {
        ReplayArm arm = rig.control;
        arm.root = control.root;
        arm.target = control.target;
        if (FAILED(RealizeWhole(arm))) return SIZE_MAX;
        return Dda::Compare(Dda::ReadBytes(extra.target.get()), Dda::ReadBytes(control.target.get()), extra.width, {})
            .differing;
    }

    SL_TEST_CASE(Stage5_MoveRepairs, Tag::Gpu)
    {
        printf("\n=== Stage 5: move ordering, clipping and bookkeeping ===\n");
        // Two frames folded into one realize: frame 1 moves M1 and paints R1,
        // frame 2 moves M2 out of R1 and paints R2. The target never got R1,
        // so M2 is drawn, not copied from it (M1 too: a second batch).
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            TEST("MoveRepair_Built", rig.ok);
            if (!rig.ok) return;
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT m1{ { 0, 60 }, { 0, 0, 30, 30 } };
            rig.Move(m1);
            const RECT r1{ 100, 10, 140, 40 };
            rig.Paint(r1, 2.2f);
            const EFFECTGRAPH_MOVE_RECT m2{ { 100, 10 }, { 40, 50, 80, 80 } };   // its source is R1
            rig.Move(m2);
            const RECT r2{ 0, 86, 20, 96 };
            rig.Paint(r2, 0.4f);
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveRepair_FoldedFramesDrawTheLaterMove", hr == S_OK && !rig.whole && rig.Drew(r1) && rig.Drew(r2) &&
                 rig.Drew(m1.DestinationRect) && rig.Drew(m2.DestinationRect) &&
                 after.movesApplied == before.movesApplied && after.movesCollapsed - before.movesCollapsed == 2 &&
                 SameMoves(rig.moved, { m1, m2 }));
            TEST("MoveRepair_FoldedFramesEqualForcedWhole", rig.DiffersFromWhole() == 0);
        }
        // Rects of one frame, a realize refused before it touches anything
        // (no `drawn`: E_INVALIDARG), then the next frame's move out of those
        // rects: the refusal changed no state, so the rects are still pending
        // and older than the move, which is drawn. (A realize that fails while
        // drawing is MoveCase_RecoveryDrawsTheDestination.)
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const RECT r1{ 100, 10, 140, 40 };
            rig.Paint(r1, 2.2f);
            UINT32 count = 0;
            const HRESULT early = rig.incremental.context.as<IEffectGraphContext2>()->RealizeToIncremental(
                rig.incremental.root.get(), rig.incremental.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, nullptr, 0,
                &count, nullptr, 0, nullptr);
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT m{ { 100, 10 }, { 40, 50, 80, 80 } };
            rig.Move(m);
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveRepair_EarlierRectsDrawTheMove", early == E_INVALIDARG && hr == S_OK && !rig.whole && rig.Drew(r1) &&
                 rig.Drew(m.DestinationRect) && after.movesApplied == before.movesApplied &&
                 after.movesCollapsed - before.movesCollapsed == 1 && SameMoves(rig.moved, { m }) &&
                 rig.DiffersFromWhole() == 0);
        }
        // Two roots on one input, both realized every frame: the first's
        // realize does not make the second's moves carry-overs.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            auto second = AddRoot(rig.incremental, L"Gamut Map", rig.width, rig.height);
            auto secondControl = AddRoot(rig.control, L"Gamut Map", rig.width, rig.height);
            const bool primed = second.ok && secondControl.ok && SUCCEEDED(second.Realize());
            uint64_t applied = 0, collapsed = 0;
            bool reported = true, current = true, incremental = true;
            const EFFECTGRAPH_MOVE_RECT moves[] = { { { 0, 0 }, { 40, 40, 80, 80 } },
                                                    { { 40, 40 }, { 90, 10, 130, 50 } },
                                                    { { 90, 10 }, { 10, 50, 50, 90 } } };
            for (const auto& m : moves)
            {
                rig.Move(m);
                current = current && SUCCEEDED(rig.Realize());
                const auto before = rig.Stats();
                const HRESULT hr = second.Realize();
                const auto after = rig.Stats();
                applied += after.movesApplied - before.movesApplied;
                collapsed += after.movesCollapsed - before.movesCollapsed;
                incremental = incremental && !second.Whole();
                reported = reported && SUCCEEDED(hr) && SameMoves(second.moved, { m });
                current = current && ExtraDiffersFromWhole(rig, second, secondControl) == 0;
            }
            printf("  [info] second root: %llu move(s) copied, %llu drawn\n", static_cast<unsigned long long>(applied),
                   static_cast<unsigned long long>(collapsed));
            TEST("MoveRepair_SecondRootCopiesToo", primed && incremental && applied == 3 && collapsed == 0 && reported);
            TEST("MoveRepair_BothRootsEqualForcedWhole", current && rig.DiffersFromWhole() == 0);
        }
        // Rects that reach a root while it is not realized: root A draws them,
        // root B does not; the next frame moves out of them, then B is
        // realized. B's target never got the rects, so B draws the move.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            auto rootB = AddRoot(rig.incremental, L"Gamut Map", rig.width, rig.height);
            auto rootBControl = AddRoot(rig.control, L"Gamut Map", rig.width, rig.height);
            const bool primed = rootB.ok && rootBControl.ok && SUCCEEDED(rootB.Realize());
            const RECT painted{ 100, 10, 140, 40 };
            rig.Paint(painted, 2.4f);
            const HRESULT a = rig.Realize();
            const EFFECTGRAPH_MOVE_RECT m{ { 105, 15 }, { 20, 50, 50, 75 } };   // its source is inside `painted`
            rig.Move(m);
            const auto before = rig.Stats();
            const HRESULT hr = rootB.Realize();
            const auto after = rig.Stats();
            auto bDrew = [&](const RECT& r)
            {
                for (LONG y = r.top; y < r.bottom; ++y)
                    for (LONG x = r.left; x < r.right; ++x)
                        if (!Dda::Inside(rootB.drawn, x, y)) return false;
                return true;
            };
            TEST("MoveRepair_UnrealizedRootDrawsTheMove", primed && SUCCEEDED(a) && hr == S_OK && !rootB.Whole() &&
                 bDrew(painted) && bDrew(m.DestinationRect) && after.movesApplied == before.movesApplied &&
                 after.movesCollapsed - before.movesCollapsed == 1 && SameMoves(rootB.moved, { m }) &&
                 ExtraDiffersFromWhole(rig, rootB, rootBControl) == 0);
        }
        // A target smaller than the input: only moves inside it are reported
        // and copied; one leaving it is drawn; one wholly outside is neither.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            auto narrow = AddRoot(rig.incremental, L"Gamut Map", 80, 48);
            auto narrowControl = AddRoot(rig.control, L"Gamut Map", 80, 48);
            const bool primed = narrow.ok && narrowControl.ok && SUCCEEDED(narrow.Realize());
            const auto before = rig.Stats();
            const EFFECTGRAPH_MOVE_RECT inside{ { 20, 20 }, { 10, 10, 40, 30 } };
            const EFFECTGRAPH_MOVE_RECT outside{ { 100, 40 }, { 100, 0, 140, 30 } };
            const EFFECTGRAPH_MOVE_RECT straddling{ { 0, 30 }, { 60, 0, 100, 20 } };
            const EFFECTGRAPH_MOVE_RECT batch[] = { inside, outside, straddling };
            rig.desktop->ApplyMoves({ inside, outside, straddling });
            rig.desktop->Upload({ inside.DestinationRect, outside.DestinationRect, straddling.DestinationRect });
            rig.input2->MoveRects(batch, 3);
            const HRESULT hr = narrow.Realize();
            const auto after = rig.Stats();
            const size_t differs = ExtraDiffersFromWhole(rig, narrow, narrowControl);
            printf("  [info] smaller target: %zu move(s) reported, %llu copied, %llu drawn, %zu px differ\n", narrow.moved.size(),
                   static_cast<unsigned long long>(after.movesApplied - before.movesApplied),
                   static_cast<unsigned long long>(after.movesCollapsed - before.movesCollapsed), differs);
            TEST("MoveRepair_SmallerTargetReportsOnlyInside", primed && hr == S_OK && !narrow.Whole() &&
                 SameMoves(narrow.moved, { inside }) && after.movesApplied - before.movesApplied == 1 &&
                 after.movesCollapsed - before.movesCollapsed == 1 && differs == 0);
        }
        // A root left unrealized: within the backlog it reports every move
        // since its last realize; past it, it draws whole and reports none.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            auto idle = AddRoot(rig.incremental, L"Gamut Map", rig.width, rig.height);
            auto idleControl = AddRoot(rig.control, L"Gamut Map", rig.width, rig.height);
            const bool primed = idle.ok && idleControl.ok && SUCCEEDED(idle.Realize());
            // A chain: each move's source is the previous one's destination,
            // so only all five, copied in order, give the right image.
            const std::vector<EFFECTGRAPH_MOVE_RECT> chain = { { { 0, 0 }, { 20, 0, 40, 20 } },
                                                               { { 20, 0 }, { 40, 10, 60, 30 } },
                                                               { { 40, 10 }, { 60, 20, 80, 40 } },
                                                               { { 60, 20 }, { 80, 30, 100, 50 } },
                                                               { { 80, 30 }, { 100, 40, 120, 60 } } };
            bool frames = true;
            for (const auto& m : chain)
            {
                rig.Move(m);
                frames = frames && SUCCEEDED(rig.Realize());
            }
            const auto beforeShort = rig.Stats();
            const HRESULT shortHr = idle.Realize();
            const auto afterShort = rig.Stats();
            TEST("MoveRepair_IdleRootReportsItsBacklog", primed && frames && shortHr == S_OK && !idle.Whole() &&
                 SameMoves(idle.moved, chain) && afterShort.movesApplied - beforeShort.movesApplied == 5 &&
                 idle.drawn.empty() && ExtraDiffersFromWhole(rig, idle, idleControl) == 0);
            const EFFECTGRAPH_MOVE_RECT swap{ { 8, 8 }, { 0, 0, 8, 8 } };
            constexpr int cFrames = 1100;   // past the 1024-generation backlog
            for (int i = 0; i < cFrames; ++i)
            {
                rig.Move(swap);
                frames = frames && SUCCEEDED(rig.Realize());
            }
            const HRESULT longHr = idle.Realize();
            TEST("MoveRepair_IdleRootPastTheBacklogDrawsWhole", frames && longHr == S_OK && idle.Whole() &&
                 idle.moved.empty() && ExtraDiffersFromWhole(rig, idle, idleControl) == 0);
            // And recovers: the next frame's move is copied again.
            const auto before = rig.Stats();
            rig.Move(swap);
            const HRESULT again = idle.Realize();
            const auto after = rig.Stats();
            TEST("MoveRepair_IdleRootRecovers", again == S_OK && !idle.Whole() && SameMoves(idle.moved, { swap }) &&
                 after.movesApplied - before.movesApplied == 1);
        }
    }

    // What keeps the move bookkeeping bounded, and what each root may lose.
    SL_TEST_CASE(Stage5_MoveBookkeeping, Tag::Gpu)
    {
        printf("\n=== Stage 5: move bookkeeping stays bounded ===\n");
        constexpr size_t cBound = 1025;    // cMaxMoveBacklog generations plus the current one, one move each
        constexpr int cCalls = 2500;
        const EFFECTGRAPH_MOVE_RECT swap{ { 8, 8 }, { 0, 0, 8, 8 } };
        // A host that forwards moves but renders with RealizeTo only: no
        // incremental realize ever prunes, MoveRects must.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            TEST("MoveBook_Built", rig.ok);
            if (!rig.ok) return;
            size_t most = 0, mostCollapsed = 0;
            bool rendered = true;
            for (int i = 0; i < cCalls; ++i)
            {
                rig.Move(swap);
                rendered = rendered && SUCCEEDED(rig.incremental.context->RealizeTo(rig.incremental.root.get(),
                                                                                  rig.incremental.target.get()));
                most = (std::max)(most, Api::Testing::g_heldMoves);
                mostCollapsed = (std::max)(mostCollapsed, Api::Testing::g_heldCollapsedMoves);
            }
            printf("  [info] RealizeTo only: %d MoveRects calls, at most %zu move(s) and %zu collapsed held\n", cCalls, most,
                   mostCollapsed);
            TEST("MoveBook_RealizeToOnlyStaysBounded", rendered && most > 0 && most <= cBound && mostCollapsed <= cBound);
            // Back to incremental: the first draws whole (RealizeTo wrote the
            // target), the next copies again.
            const HRESULT first = rig.Realize();
            const bool firstWhole = rig.whole;
            const auto before = rig.Stats();
            rig.Move({ { 30, 30 }, { 60, 40, 100, 70 } });
            const HRESULT next = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveBook_RealizeToOnlyRecovers", first == S_OK && firstWhole && next == S_OK && !rig.whole &&
                 after.movesApplied - before.movesApplied == 1 && rig.DiffersFromWhole() == 0);
        }
        // The only incremental root keeps failing: its failed realizes prune
        // too, and past the backlog it draws whole once it recovers.
        {
            auto rig = MakeMoveRig(L"ICtCp Gamut Map");
            TEST("MoveBook_FaultyBuilt", rig.ok);
            if (!rig.ok) return;
            FaultyHooks hooks;
            rig.incremental.context->Evaluator().SetEffectHooks(&hooks);
            struct RestoreHooks
            {
                IEffectGraphContextInternal* context;
                ~RestoreHooks() { context->Evaluator().SetEffectHooks(&Effects::BuiltInEffectHooks()); }
            } restore{ rig.incremental.context.get() };
            hooks.failing = true;
            size_t most = 0;
            bool allFailed = true;
            for (int i = 0; i < cCalls; ++i)
            {
                rig.Move(swap);
                allFailed = allFailed && FAILED(rig.Realize());
                most = (std::max)(most, Api::Testing::g_heldMoves);
            }
            hooks.failing = false;
            const HRESULT recovered = rig.Realize();
            printf("  [info] failing root: %d failed realizes, at most %zu move(s) held; recovery %s\n", cCalls, most,
                   rig.whole ? "drew whole" : "drew rects");
            TEST("MoveBook_FailingRootStaysBounded", allFailed && most > 0 && most <= cBound);
            TEST("MoveBook_FailingRootPastTheBacklogDrawsWhole", recovered == S_OK && rig.whole && rig.moved.empty() &&
                 rig.DiffersFromWhole() == 0);
        }
        // A root that reads only a static input does not lose anything to
        // moves on another input, however many generations pass.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            Dda::Desktop still(g_d3dDevice.get(), g_d3dContext.get(), 64, 48);
            for (uint32_t y = 0; y < 48; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                    still.Put(x, y, 0.1f + x / 64.0f, 0.2f, 0.05f + y / 48.0f);
            still.UploadAll();
            ExtraRoot other;
            other.width = 64;
            other.height = 48;
            other.context2 = rig.incremental.context.as<IEffectGraphContext2>();
            winrt::com_ptr<IEffectGraphNode> stillInput;
            other.ok = SUCCEEDED(rig.incremental.context->CreateTextureInput(still.Texture(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                            stillInput.put())) &&
                       SUCCEEDED(rig.incremental.context->CreateEffect(L"Gamut Map", other.root.put())) &&
                       SUCCEEDED(other.root->SetInput(0, stillInput.get()));
            other.target = MakeSrgbTarget(64, 48);
            const bool primed = other.ok && SUCCEEDED(other.Realize()) && other.Whole();
            bool frames = true;
            for (int i = 0; i < 1100; ++i)
            {
                rig.Move(swap);
                frames = frames && SUCCEEDED(rig.Realize());
            }
            const auto before = rig.Stats();
            const HRESULT hr = other.Realize();
            const auto after = rig.Stats();
            TEST("MoveBook_UnrelatedMovesCostNoWholeDraw", primed && frames && hr == S_OK && !other.Whole() &&
                 other.drawn.empty() && other.moved.empty() && after.wholeDraws == before.wholeDraws);
        }
        // `moved` with no `movedCount`: refused, and nothing changes.
        {
            auto rig = MakeMoveRig(L"Gamut Map");
            const EFFECTGRAPH_MOVE_RECT m{ { 0, 0 }, { 60, 30, 100, 70 } };
            rig.Move(m);
            RECT rects[16]{};
            EFFECTGRAPH_MOVE_RECT moves[16]{};
            UINT32 count = 0;
            const auto before = rig.Stats();
            const HRESULT refused = rig.incremental.context.as<IEffectGraphContext2>()->RealizeToIncremental(
                rig.incremental.root.get(), rig.incremental.target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, rects, 16,
                &count, moves, 16, nullptr);
            const auto mid = rig.Stats();
            const HRESULT hr = rig.Realize();
            const auto after = rig.Stats();
            TEST("MoveBook_MovedWithoutCountRefused", rig.ok && refused == E_INVALIDARG && mid.realizes == before.realizes &&
                 hr == S_OK && after.movesApplied - mid.movesApplied == 1 && SameMoves(rig.moved, { m }) &&
                 rig.DiffersFromWhole() == 0);
        }
    }

    // A move while the root is compile-pending, then recovery: the realizes
    // in between fail, and the first good one draws the move's destination.
    SL_TEST_CASE(Stage5_MoveWhileCompilePending, Tag::Gpu)
    {
        printf("\n=== Stage 5: a move while the root is compile-pending ===\n");
        constexpr uint32_t cWidth = 160, cHeight = 96;
        auto rig = MakeMoveRig(L"Gamut Map", cWidth, cHeight);
        // The stage's context, with async and runtime compile on.
        ReplayArm pending;
        EFFECTGRAPH_CONTEXT_DESC desc{ static_cast<EFFECTGRAPH_CONTEXT_FLAGS>(EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE |
                                                                              EFFECTGRAPH_CONTEXT_FLAGS_ALLOW_RUNTIME_COMPILE),
                                       nullptr };
        pending.ok = SUCCEEDED(EffectGraphCreateContext(g_d3dDevice.get(), &desc, __uuidof(IEffectGraphContextInternal),
                                                        pending.context.put_void())) &&
                     SUCCEEDED(pending.context->CreateTextureInput(rig.desktop->Texture(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                   pending.input.put())) &&
                     SUCCEEDED(pending.context->CreateEffect(L"Gamut Map", pending.root.put())) &&
                     SUCCEEDED(pending.root->SetInput(0, pending.input.get()));
        TEST("MovePending_Built", rig.ok && pending.ok);
        if (!rig.ok || !pending.ok) return;
        SetShiftInvariant(pending, true);
        pending.target = MakeSrgbTarget(cWidth, cHeight);
        auto context2 = pending.context.as<IEffectGraphContext2>();
        auto input2 = pending.input.as<IEffectGraphInput2>();
        std::vector<RECT> drawn;
        std::vector<EFFECTGRAPH_MOVE_RECT> moved;
        const auto realize = [&]
        {
            RECT rects[16]{};
            EFFECTGRAPH_MOVE_RECT moves[16]{};
            UINT32 count = 0, movedCount = 0;
            const HRESULT hr = context2->RealizeToIncremental(pending.root.get(), pending.target.get(),
                EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, rects, 16, &count, moves, 16, &movedCount);
            drawn.assign(rects, rects + (SUCCEEDED(hr) ? count : 0));
            moved.assign(moves, moves + (SUCCEEDED(hr) ? movedCount : 0));
            return hr;
        };
        const auto untilDrawn = [&]
        {
            const auto start = std::chrono::steady_clock::now();
            HRESULT hr = realize();
            while (FAILED(hr) && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                hr = realize();
            }
            return hr;
        };
        const bool primed = SUCCEEDED(untilDrawn());
        // A good move first: copied.
        const auto stats0 = pending.context->Evaluator().RegionStats();
        const EFFECTGRAPH_MOVE_RECT good{ { 0, 0 }, { 40, 40, 80, 80 } };
        rig.desktop->ApplyMoves({ good });
        rig.desktop->Upload({ good.DestinationRect });
        input2->MoveRects(&good, 1);
        const HRESULT goodHr = realize();
        const auto stats1 = pending.context->Evaluator().RegionStats();
        TEST("MovePending_GoodMoveCopied", primed && goodHr == S_OK && stats1.movesApplied - stats0.movesApplied == 1);

        // A new baseline the context compiles in the background (white-box:
        // only a comment, so the node renders the same once compiled).
        auto* root = pending.context->Model().FindNode(pending.context->NodeId(pending.root.get()));
        root->customEffect->hlslSource += L"\n// compile-pending move " +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
        root->customEffect->compiledBytecode.clear();
        const EFFECTGRAPH_MOVE_RECT move{ { 90, 10 }, { 10, 10, 50, 40 } };
        rig.desktop->ApplyMoves({ move });
        rig.desktop->Upload({ move.DestinationRect });
        input2->MoveRects(&move, 1);
        const HRESULT waiting = realize();
        const auto stats2 = pending.context->Evaluator().RegionStats();
        const HRESULT recovered = untilDrawn();
        const auto stats3 = pending.context->Evaluator().RegionStats();
        bool destinationDrawn = true;
        for (LONG y = move.DestinationRect.top; y < move.DestinationRect.bottom; ++y)
            for (LONG x = move.DestinationRect.left; x < move.DestinationRect.right; ++x)
                if (!Dda::Inside(drawn, x, y)) destinationDrawn = false;
        RealizeWhole(rig.control);
        const auto diff = Dda::Compare(Dda::ReadBytes(pending.target.get()), Dda::ReadBytes(rig.control.target.get()), cWidth, {});
        const bool recoveredWhole = stats3.wholeDraws > stats2.wholeDraws;
        printf("  [info] compile pending: %s, then recovered %s (%s); %llu collapsed; %zu px differ from the forced-whole "
               "render\n", waiting == EFFECTGRAPH_E_COMPILE_PENDING ? "COMPILE_PENDING" : "another result",
               recovered == S_OK ? "S_OK" : "with an error", recoveredWhole ? "drawn whole" : "incremental",
               static_cast<unsigned long long>(stats3.movesCollapsed - stats2.movesCollapsed), diff.differing);
        TEST("MovePending_CompilePendingReturned", waiting == EFFECTGRAPH_E_COMPILE_PENDING);
        // Incremental: a whole draw would draw the destination without the rule.
        TEST("MovePending_RecoveryDrawsTheDestination", recovered == S_OK && destinationDrawn && !recoveredWhole &&
             stats3.movesCollapsed - stats2.movesCollapsed == 1 && stats3.movesApplied == stats2.movesApplied);
        TEST("MovePending_RecoveryReportsTheMove", SameMoves(moved, { move }));
        TEST("MovePending_RecoveryEqualsForcedWhole", diff.differing == 0);
    }

    // A commit that lands on a frame with moves wakes the tone map whole: the
    // frame draws whole and copies nothing, and still reports the moves.
    SL_TEST_CASE(Stage5_CommitWithMovesDrawsWhole, Tag::Gpu)
    {
        printf("\n=== Stage 5: a commit on a frame with moves ===\n");
        RigOptions o;
        o.maxPolicy = EFFECTGRAPH_COMMIT_POLICY_HOSTED;
        o.budgetPolicy = EFFECTGRAPH_COMMIT_POLICY_THRESHOLD;
        o.standIn = true;
        auto rig = MakeDeferredRig(o);
        TEST("MoveCommit_Built", rig.ok);
        if (!rig.ok) return;
        const auto* root = rig.Model().FindNode(rig.rootId);
        TEST("MoveCommit_StandInShiftInvariant", root && root->customEffect->shiftInvariant);
        std::vector<EFFECTGRAPH_MOVE_RECT> moved;
        const auto realize = [&]
        {
            RECT rects[16]{};
            EFFECTGRAPH_MOVE_RECT moves[16]{};
            UINT32 count = 0, movedCount = 0;
            const HRESULT hr = rig.context2->RealizeToIncremental(rig.root.get(), rig.target.get(),
                EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, rects, 16, &count, moves, 16, &movedCount);
            rig.drawn.assign(rects, rects + (SUCCEEDED(hr) ? count : 0));
            moved.assign(moves, moves + (SUCCEEDED(hr) ? movedCount : 0));
            rig.whole = rig.drawn.size() == 1 && rig.drawn[0].left == 0 && rig.drawn[0].top == 0 &&
                        rig.drawn[0].right == static_cast<LONG>(rig.width) && rig.drawn[0].bottom == static_cast<LONG>(rig.height);
            return hr;
        };
        const auto settle = [&]
        {
            HRESULT hr = S_FALSE;
            for (int i = 0; i < 16 && hr == S_FALSE; ++i)
            {
                WaitForGpuIdle();
                hr = realize();
            }
            return hr;
        };
        const auto move = [&](const EFFECTGRAPH_MOVE_RECT& m)
        {
            rig.desktop->ApplyMoves({ m });
            rig.desktop->Upload({ m.DestinationRect });
            rig.input2->MoveRects(&m, 1);
        };
        const bool primed = settle() == S_OK;
        // The control: a move with nothing to commit is copied.
        auto stats = rig.Evaluator().RegionStats();
        const EFFECTGRAPH_MOVE_RECT first{ { 20, 20 }, { 30, 30, 50, 50 } };
        move(first);
        const HRESULT copiedHr = settle();
        auto after = rig.Evaluator().RegionStats();
        TEST("MoveCommit_ControlMoveCopied", primed && copiedHr == S_OK && !rig.whole &&
                                             after.movesApplied - stats.movesApplied == 1);
        // The host commits a new peak on the frame of the next move.
        stats = rig.Evaluator().RegionStats();
        const auto copiedBefore = stats.movePixelsCopied;
        const EFFECTGRAPH_MOVE_RECT second{ { 30, 30 }, { 40, 10, 60, 30 } };
        move(second);
        const float peak = 1700.0f;
        const HRESULT committed = rig.stats2->CommitFields(&rig.maxIndex, &peak, 1);
        const HRESULT hr = realize();
        after = rig.Evaluator().RegionStats();
        printf("  [info] commit with a move: %s, %zu rect(s) drawn%s, %llu move(s) copied, %zu reported\n",
               SUCCEEDED(hr) ? "realized" : "failed", rig.drawn.size(), rig.whole ? " (whole)" : "",
               static_cast<unsigned long long>(after.movesApplied - stats.movesApplied), moved.size());
        TEST("MoveCommit_DrawsWhole", committed == S_OK && SUCCEEDED(hr) && rig.whole);
        TEST("MoveCommit_NoScratchCopies", after.movesApplied == stats.movesApplied &&
                                           after.movePixelsCopied == copiedBefore);
        TEST("MoveCommit_StillReportsTheMove", SameMoves(moved, { second }));
        TEST("MoveCommit_Committed", Near(rig.Bound(L"SourcePeakNits"), peak, 0.01f));
    }

    // ---- The numbers: two copies of each moved destination against one
    // tone map of it. The same frames with applyMoves on and off, each present
    // from an idle GPU; both draw the frame's dirty rects, so the difference is
    // the destinations' re-tone-map against their copies. The client copy is
    // the same in both and not counted (no client here).
    SL_TEST_CASE(Stage5_MoveCost, Tag::Gpu)
    {
        printf("\n=== Stage 5: what a move costs (%ls) ===\n", MoveChainEffect());
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        presents = g_useWarp ? 12 : 30;
        GpuStopwatch stopwatch;
        for (const auto kind : { Dda::ScenarioKind::Scroll, Dda::ScenarioKind::WindowDrag })
        {
            const std::string name = std::string("MoveCost_") + Dda::ScenarioName(kind);
            auto scenario = Dda::MakeScenario(kind, presents);
            Dda::Desktop desktop(g_d3dDevice.get(), g_d3dContext.get(), width, height);
            const auto source = SyntheticSource(*scenario);
            source.start(desktop);
            ReplayArm arms[2] = { MakeArm(desktop.Texture(), MoveChainEffect(), ArmKind::Incremental),
                                  MakeArm(desktop.Texture(), MoveChainEffect(), ArmKind::Incremental) };
            if (!arms[0].ok || !arms[1].ok)
            {
                TEST((name + "_Built").c_str(), false);
                continue;
            }
            if (g_pluginPath.empty())
                for (auto& arm : arms) SetShiftInvariant(arm, true);
            auto offOptions = arms[1].context->Evaluator().Options();
            offOptions.applyMoves = false;
            arms[1].context->Evaluator().SetOptions(offOptions);
            std::unique_ptr<Dda::StageSim> stages[2];
            for (int a = 0; a < 2; ++a)
            {
                stages[a] = std::make_unique<Dda::StageSim>(arms[a].context.get(), arms[a].input.get(), arms[a].root.get(),
                                                            arms[a].target.get(), g_d3dContext.get());
                stages[a]->SetMoves(true);
                stages[a]->Prime();
            }
            // GPU timestamps, and wall-clock ms from an idle GPU to idle again
            // (submission included): the instrument check on the timestamps.
            std::vector<double> ms[2], wallMs[2];
            uint64_t applied[2]{}, collapsed[2]{}, handed = 0, pixelsMoved = 0;
            uint32_t notLive[2]{};
            const uint32_t rootIds[2] = { arms[0].context->NodeId(arms[0].root.get()),
                                          arms[1].context->NodeId(arms[1].root.get()) };
            Dda::DesktopFrame frame;
            uint32_t index = 0;
            while (source.next(desktop, frame))
            {
                if (frame.kind == Dda::FrameKind::NoFrame) continue;
                handed += frame.moves.size();
                for (const auto& m : frame.moves)
                    pixelsMoved += static_cast<uint64_t>(m.DestinationRect.right - m.DestinationRect.left) *
                                   static_cast<uint64_t>(m.DestinationRect.bottom - m.DestinationRect.top);
                // Alternate which arm goes first.
                for (int k = 0; k < 2; ++k)
                {
                    const int a = (index + k) % 2;
                    const auto before = arms[a].context->Evaluator().RegionStats();
                    const uint64_t changes = arms[a].context->Evaluator().OutputChangeCount(rootIds[a]);
                    WaitForGpuIdle();
                    const auto wallStart = std::chrono::steady_clock::now();
                    stopwatch.Begin();
                    stages[a]->Present(frame);
                    ms[a].push_back(stopwatch.End());
                    WaitForGpuIdle();
                    wallMs[a].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count());
                    const auto& now = arms[a].context->Evaluator().RegionStats();
                    applied[a] += now.movesApplied - before.movesApplied;
                    collapsed[a] += now.movesCollapsed - before.movesCollapsed;
                    if (arms[a].context->Evaluator().OutputChangeCount(rootIds[a]) == changes) ++notLive[a];
                }
                ++index;
            }
            // The two copies alone, timed on the on-arm's target through a scratch.
            std::vector<double> copyMs, copyWallMs;
            uint64_t copyPixels = 0;
            {
                const EFFECTGRAPH_MOVE_RECT m = kind == Dda::ScenarioKind::Scroll
                    ? EFFECTGRAPH_MOVE_RECT{ { static_cast<LONG>(width / 8), static_cast<LONG>(height / 8 + 16) },
                                             { static_cast<LONG>(width / 8), static_cast<LONG>(height / 8),
                                               static_cast<LONG>(width * 5 / 8), static_cast<LONG>(height * 7 / 8 - 16) } }
                    : EFFECTGRAPH_MOVE_RECT{ { 0, 0 }, { 12, 7, 12 + static_cast<LONG>(width * 33 / 100),
                                                         7 + static_cast<LONG>(height * 33 / 100) } };
                const LONG w = m.DestinationRect.right - m.DestinationRect.left, h = m.DestinationRect.bottom - m.DestinationRect.top;
                copyPixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
                D3D11_TEXTURE2D_DESC desc{};
                arms[0].target->GetDesc(&desc);
                desc.Width = static_cast<UINT>(w);
                desc.Height = static_cast<UINT>(h);
                desc.BindFlags = 0;
                desc.MiscFlags = 0;
                winrt::com_ptr<ID3D11Texture2D> scratch;
                winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, scratch.put()));
                const D3D11_BOX from{ static_cast<UINT>(m.SourcePoint.x), static_cast<UINT>(m.SourcePoint.y), 0,
                                      static_cast<UINT>(m.SourcePoint.x + w), static_cast<UINT>(m.SourcePoint.y + h), 1 };
                const D3D11_BOX all{ 0, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 1 };
                for (int rep = 0; rep < 12; ++rep)
                {
                    WaitForGpuIdle();
                    const auto wallStart = std::chrono::steady_clock::now();
                    stopwatch.Begin();
                    g_d3dContext->CopySubresourceRegion(scratch.get(), 0, 0, 0, 0, arms[0].target.get(), 0, &from);
                    g_d3dContext->CopySubresourceRegion(arms[0].target.get(), 0, m.DestinationRect.left,
                                                        m.DestinationRect.top, 0, scratch.get(), 0, &all);
                    const double t = stopwatch.End();
                    WaitForGpuIdle();
                    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count();
                    if (rep >= 2)
                    {
                        copyMs.push_back(t);
                        copyWallMs.push_back(wall);
                    }
                }
            }
            const double on = Median(ms[0]), off = Median(ms[1]);
            printf("  [info] %s, %u x %u, %s: GPU ms per present from an idle GPU, median (mean): moves copied %.3f (%.3f), "
                   "moves drawn %.3f (%.3f); two copies of one destination alone %.3f; %llu px moved over %u presents\n",
                   name.c_str(), width, height, g_useWarp ? "WARP: not a hardware number" : "hardware", on, Mean(ms[0]),
                   off, Mean(ms[1]), Median(copyMs), static_cast<unsigned long long>(pixelsMoved), index);
            // Two copies move each pixel's 4 bytes in and out twice.
            const double copyBytes = static_cast<double>(copyPixels) * 16.0;
            const double timestampGBs = copyBytes / (Median(copyMs) * 1e6), wallGBs = copyBytes / (Median(copyWallMs) * 1e6);
            printf("  [info] %s: wall clock, idle to idle (submission included), median: moves copied %.3f ms, moves drawn "
                   "%.3f ms; the two copies alone %.3f ms (%llu px: %.0f GB/s by timestamps, %.0f GB/s by the wall clock)\n",
                   name.c_str(), Median(wallMs[0]), Median(wallMs[1]), Median(copyWallMs),
                   static_cast<unsigned long long>(copyPixels), timestampGBs, wallGBs);
            if (timestampGBs > 1000.0)
                printf("  [info] %s: instrument: the timestamps imply over 1 TB/s for the copies, so they do not bracket "
                       "the copy work on this adapter; read the wall-clock figures\n", name.c_str());
            printf("  [info] %s: instrument: on arm copied %llu / drew %llu moves, off arm copied %llu / drew %llu; "
                   "presents with the root unchanged %u / %u\n", name.c_str(),
                   static_cast<unsigned long long>(applied[0]), static_cast<unsigned long long>(collapsed[0]),
                   static_cast<unsigned long long>(applied[1]), static_cast<unsigned long long>(collapsed[1]),
                   notLive[0], notLive[1]);
            TEST((name + "_Instrument").c_str(), index > 0 && handed > 0 && applied[0] == handed && collapsed[0] == 0 &&
                                                 applied[1] == 0 && collapsed[1] == handed && notLive[0] == 0 && notLive[1] == 0);
            // Each present times both arms back to back (order alternating).
            // The timestamps do not bracket the copies on every adapter, so
            // the claim is read on the wall clock, as a paired comparison.
            const auto wall = ComparePaired(wallMs[0], wallMs[1]);
            PrintPairedTiming((name + " wall clock, copied - drawn").c_str(), wall);
            // Only the real tone map (--plugin) costs enough to claim the
            // copies are cheaper: the stand-in costs about what two copies'
            // submission does, so without the plugin the figures are reported.
            if (!g_useWarp && !g_pluginPath.empty())
                TIMING_TEST((name + "_CopiesCheaperThanToneMap").c_str(), wall.ConfidentlyBelow());
            else if (!g_useWarp)
                printf("  [info] %s_CopiesCheaperThanToneMap: needs --plugin (the stand-in tone map is about the cost "
                       "of the copies), reported above, not asserted\n", name.c_str());
        }
    }

    // The stage 3 gate with moves: Stage3Replay with the stage and drain arms
    // handing the engine their moves.
    SL_TEST_CASE(Stage5_Stage3GateWithMoves, Tag::Gpu)
    {
        printf("\n=== Stage 5: the stage 3 scenario gate with moves (%ls) ===\n",
               g_pluginPath.empty() ? L"Stage2 Gate Tone Map" : cTonemapName);
        uint32_t width, height, presents;
        ReplaySize(width, height, presents);
        for (const auto kind : { Dda::ScenarioKind::Scroll, Dda::ScenarioKind::WindowDrag,
                                 Dda::ScenarioKind::VideoAndScroll, Dda::ScenarioKind::HdrHighlights })
        {
            auto scenario = Dda::MakeScenario(kind, presents);
            const auto result = Stage3Replay(width, height, SyntheticSource(*scenario), true);
            ReportStage3(std::string("Stage5Gate_") + Dda::ScenarioName(kind), result);
            printf("  [info] Stage5Gate_%s: %llu move(s) copied, %llu drawn at their destinations by the stage arm; "
                   "%zu compared pixel(s) within %d codes of the whole render (moved dither)\n",
                   Dda::ScenarioName(kind), static_cast<unsigned long long>(result.movesApplied),
                   static_cast<unsigned long long>(result.movesCollapsed), result.surfaceDrift, Rendering::cMoveDitherCodes);
            // The stand-in does not dither: its copied pixels are exact.
            if (g_pluginPath.empty())
                TEST((std::string("Stage5Gate_") + Dda::ScenarioName(kind) + "_ExactWithoutDither").c_str(),
                     result.surfaceDrift == 0);
            if (kind != Dda::ScenarioKind::HdrHighlights)
                TEST((std::string("Stage5Gate_") + Dda::ScenarioName(kind) + "_MovesCopied").c_str(), result.movesApplied > 0);
        }
    }
}
