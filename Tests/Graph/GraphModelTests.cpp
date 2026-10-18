#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/EffectRegistry.h"
#include "Engine/Utf8.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/SourceRuntime.h"
#include "Graph/GraphEdits.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Graph model, frame semantics, serialization, bindings, and parameter and
// clock nodes.
namespace
{
    using namespace ShaderLab::Tests;

    SL_TEST_CASE(TestGraphOperations, Tag::Fast)
    {
        printf("\n=== Graph Operations ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::GraphModel g;

        // Add/remove.
        auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto id1 = g.AddNode(std::move(node));
        TEST("AddNode", g.FindNode(id1) != nullptr);
        TEST("NodeCount", g.Nodes().size() == 1);
        g.RemoveNode(id1);
        TEST("RemoveNode", g.FindNode(id1) == nullptr && g.Nodes().empty());

        // Connect/disconnect.
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        TEST("Connect", g.Connect(srcId, 0, blurId, 0));
        TEST("EdgeExists", g.GetInputEdges(blurId).size() == 1);
        g.Disconnect(srcId, 0, blurId, 0);
        TEST("Disconnect", g.GetInputEdges(blurId).empty());

        // Topo sort.
        g.Connect(srcId, 0, blurId, 0);
        auto topo = g.TopologicalSort();
        TEST("TopoSort", topo.size() == 2 && topo[0] == srcId);

        // Cycle detection.
        TEST("DetectsCycle", g.WouldCreateCycle(blurId, srcId));
        TEST("AllowsExisting", !g.WouldCreateCycle(srcId, blurId));
    }

    // Graph-side frame semantics: one dependency definition behind the sort,
    // the cycle check, dirty propagation and demand; the clock model; and the
    // FrameExecutor contract every host renders through.
    SL_TEST_CASE(TestFrameSemantics, Tag::Gpu)
    {
        printf("\n=== Frame semantics ===\n");
        using ShaderLab::Graph::DependencyKind;
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* gamutDesc = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* gainDesc = registry.FindByName(L"ICtCp Round-Trip Validator");
        auto* clockDesc = registry.FindByName(L"Clock");
        if (!gamutDesc || !statsDesc || !gainDesc || !clockDesc)
        {
            TEST("Frame_FindDescriptors", false);
            return;
        }
        auto mk = [](ShaderLab::Graph::GraphModel& g, const auto* desc)
        {
            return g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc));
        };

        // a -> stats, b -> gain, gain.Gain <- stats.Max, c unconnected.
        {
            ShaderLab::Graph::GraphModel g;
            const auto a = mk(g, gamutDesc), b = mk(g, gamutDesc), c = mk(g, gamutDesc);
            const auto stats = mk(g, statsDesc), gain = mk(g, gainDesc);
            g.Connect(a, 0, stats, 0);
            g.Connect(b, 0, gain, 0);
            TEST("Frame_BindForDependencyTests", g.BindProperty(gain, L"Gain", stats, L"Max", 0).empty());

            int image = 0, binding = 0, any = 0;
            g.ForEachDependency(DependencyKind::Image, [&](uint32_t, uint32_t) { ++image; });
            g.ForEachDependency(DependencyKind::Binding, [&](uint32_t src, uint32_t dst)
                { if (src == stats && dst == gain) ++binding; });
            g.ForEachDependency(DependencyKind::Any, [&](uint32_t, uint32_t) { ++any; });
            TEST("Deps_KindsPartitionTheEdges", image == 2 && binding == 1 && any == 3);

            // gain -> stats closes stats ~> gain (binding) -> stats (edge).
            TEST("Cycle_ConnectThroughBindingRejected", !g.Connect(gain, 0, stats, 0));
            TEST("Cycle_SortStillOrdersBindings", [&] {
                const auto order = g.TopologicalSort();
                auto at = [&](uint32_t id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
                return order.size() == 5 && at(a) < at(stats) && at(stats) < at(gain);
            }());

            for (auto& n : g.MutableNodes()) n.dirty = false;
            ShaderLab::Graph::MarkNodeChanged(*g.FindNode(a));
            g.PropagateDirtyDownstream();
            TEST("Dirty_PropagateFollowsEdges", g.FindNode(stats)->dirty);
            TEST("Dirty_PropagateLeavesBindingConsumers", !g.FindNode(gain)->dirty && !g.FindNode(b)->dirty);

            for (auto& n : g.MutableNodes()) n.dirty = false;
            const uint32_t roots[] = { gain };
            g.ComputeNeeded(roots);
            TEST("Demand_RootAndItsEdges", g.FindNode(gain)->needed && g.FindNode(b)->needed);
            TEST("Demand_FollowsBindings", g.FindNode(stats)->needed && g.FindNode(a)->needed);
            TEST("Demand_SkipsUnrelated", !g.FindNode(c)->needed);
            ShaderLab::Graph::MarkNodeChanged(*g.FindNode(stats));
            g.ComputeNeeded({});
            TEST("Demand_DirtyAnalysisDemandsItself",
                 g.FindNode(stats)->needed && g.FindNode(a)->needed && !g.FindNode(gain)->needed);
            g.MarkAllNeeded();
            TEST("Demand_AllNeeded", g.FindNode(c)->needed && g.FindNode(gain)->needed);
        }

        // Clock model.
        {
            ShaderLab::Graph::GraphModel g;
            const auto clockId = mk(g, clockDesc);
            auto* clock = g.FindNode(clockId);
            ShaderLab::Graph::SetNodeProperty(*clock, L"AutoDuration", 0.0f);
            ShaderLab::Graph::SetNodeProperty(*clock, L"StartTime", 0.0f);
            ShaderLab::Graph::SetNodeProperty(*clock, L"StopTime", 2.0f);
            ShaderLab::Graph::SetNodeProperty(*clock, L"Loop", 1.0f);
            clock->clockTime = 1.5;
            TEST("Clock_AdvanceEmits", ShaderLab::Graph::AdvanceClock(*clock, 1.0));
            TEST("Clock_LoopWraps", std::abs(clock->clockTime - 0.5) < 1e-12);

            ShaderLab::Graph::SetNodeProperty(*clock, L"Loop", 0.0f);
            ShaderLab::Graph::SetClockPlaying(*clock, true);
            ShaderLab::Graph::AdvanceClock(*clock, 5.0);
            TEST("Clock_NonLoopStopsAtEnd", clock->clockTime == 2.0 && !clock->isPlaying);

            ShaderLab::Graph::SetNodeProperty(*clock, L"Loop", 1.0f);
            ShaderLab::Graph::SetNodeProperty(*clock, L"UpdateRate", 10.0f);
            clock->clockTime = 0.0;
            clock->clockTickBucket = -1;
            const bool first = ShaderLab::Graph::AdvanceClock(*clock, 0.01);
            const bool sameBucket = ShaderLab::Graph::AdvanceClock(*clock, 0.01);
            const bool nextBucket = ShaderLab::Graph::AdvanceClock(*clock, 0.1);
            TEST("Clock_UpdateRateGatesDirty", first && !sameBucket && nextBucket);
            ShaderLab::Graph::SetNodeProperty(*clock, L"UpdateRate", 0.0f);

            // AutoDuration: StopTime follows the Duration of a node bound to the clock.
            const auto consumerId = mk(g, gamutDesc);
            auto* consumer = g.FindNode(consumerId);
            ShaderLab::Graph::PropertyBinding binding;
            binding.sources.push_back(ShaderLab::Graph::ComponentSource{ clockId, L"Time", 0, 0 });
            consumer->propertyBindings[L"Time"] = binding;
            ShaderLab::Graph::AnalysisFieldValue duration;
            duration.name = L"Duration";
            duration.type = ShaderLab::Graph::AnalysisFieldType::Float;
            duration.components[0] = 4.0f;
            consumer->analysisOutput.fields.push_back(duration);
            clock = g.FindNode(clockId);
            ShaderLab::Graph::SetNodeProperty(*clock, L"AutoDuration", 1.0f);
            ShaderLab::Graph::SetClockPlaying(*clock, false);
            ShaderLab::Graph::AdvanceClocks(g, 0.1);
            const auto* stop = std::get_if<float>(&clock->properties[L"StopTime"]);
            TEST("Clock_AutoDurationFollowsBoundDuration", stop && *stop == 4.0f);
            clock->propertyBindings[L"StopTime"] = binding;
            ShaderLab::Graph::AdvanceClocks(g, 0.1);
            const auto* autoDuration = std::get_if<float>(&clock->properties[L"AutoDuration"]);
            TEST("Clock_BoundStopTimeDisablesAutoDuration", autoDuration && *autoDuration == 0.0f);
        }

        // Video seek policy.
        {
            ShaderLab::Graph::EffectNode video;
            ShaderLab::Graph::SetNodeProperty(video, L"Time", 5.5f);
            ShaderLab::Graph::SetNodeProperty(video, L"Loop", true);
            using Factory = ShaderLab::Effects::SourceNodeFactory;
            const auto looped = Factory::VideoTargetTime(video, 2.0);
            TEST("VideoTime_LoopWraps", looped && std::abs(*looped - 1.5) < 1e-9);
            ShaderLab::Graph::SetNodeProperty(video, L"Loop", false);
            TEST("VideoTime_PastEndIsNothing", !Factory::VideoTargetTime(video, 2.0).has_value());
            ShaderLab::Graph::SetNodeProperty(video, L"Time", -1.0f);
            const auto clamped = Factory::VideoTargetTime(video, 2.0);
            TEST("VideoTime_NegativeClampsToStart", clamped && *clamped == 0.0);
            ShaderLab::Graph::SetNodeProperty(video, L"Time", 7.0f);
            const auto unknown = Factory::VideoTargetTime(video, 0.0);
            TEST("VideoTime_UnknownDurationNoWrap", unknown && *unknown == 7.0);
        }

        // FrameExecutor and the frozen evaluation mode.
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel g;
            const auto src = mk(g, gamutDesc);
            const auto stats = mk(g, statsDesc);
            const auto unrelated = mk(g, gamutDesc);
            g.Connect(src, 0, stats, 0);

            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(g, g_dc.get(), ShaderLab::Rendering::GraphEvaluator::EvalMode::Frozen);
            TEST("EvalMode_FrozenQueuesNoCompute", CaseEvaluator().DeferredComputeCount() == 0);

            winrt::com_ptr<ID2D1Bitmap1> target;
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            g_dc->CreateBitmap(D2D1::SizeU(16, 16), nullptr, 0, props, target.put());
            winrt::com_ptr<ID2D1Bitmap1> hostTarget;
            g_dc->CreateBitmap(D2D1::SizeU(4, 4), nullptr, 0, props, hostTarget.put());
            g_dc->SetTarget(hostTarget.get());

            ShaderLab::Rendering::FrameRequest request;
            request.target = target.get();
            request.markAllDirty = true;
            request.demand = ShaderLab::Rendering::FrameDemand::Roots;
            request.roots = { stats };
            bool drawnInSession = false;
            request.draw = [&](ID2D1DeviceContext5* ctx)
            {
                winrt::com_ptr<ID2D1Image> bound;
                ctx->GetTarget(bound.put());
                drawnInSession = bound.get() == target.get();
                ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
            };
            ShaderLab::Rendering::FrameResult frame{};
            for (int i = 0; i < 2; ++i)
                frame = ShaderLab::Rendering::FrameExecutor::Run(g, CaseEvaluator(), g_dc.get(), request);
            winrt::com_ptr<ID2D1Image> after;
            g_dc->GetTarget(after.put());
            g_dc->SetTarget(nullptr);

            const float statsMax = AnalysisField(g.FindNode(stats), L"Max");
            TEST("Frame_EndDrawSucceeded", SUCCEEDED(frame.endDraw));
            TEST("Frame_ComputeDispatched", frame.computeRan && frame.dispatches >= 1 && statsMax > 0.0f);
            TEST("Frame_DrawRunsOnTargetInSession", drawnInSession);
            TEST("Frame_RestoresHostTarget", after.get() == hostTarget.get());
            TEST("Frame_DemandSkipsUnrelated", !g.FindNode(unrelated)->needed && g.FindNode(src)->needed);
            TEST("Frame_LeavesNothingDirty", !g.HasDirtyNodes());
            CaseEvaluator().ReleaseCache();
        }
    }

    SL_TEST_CASE(TestSerialization, Tag::Gpu)
    {
        printf("\n=== Serialization ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::GraphModel g;
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        g.Connect(srcId, 0, blurId, 0);
        ShaderLab::Graph::SetNodeProperty(*g.FindNode(blurId), L"StandardDeviation", 3.0f);

        auto json = g.ToJson();
        TEST("SerializeNotEmpty", !json.empty());

        auto g2 = ShaderLab::Graph::GraphModel::FromJson(json);
        TEST("DeserializeNodeCount", g2.Nodes().size() == 2);
        TEST("DeserializeEdges", !g2.Edges().empty());

        auto* blurLoaded = g2.FindNode(blurId);
        bool propOk = false;
        if (blurLoaded) {
            auto it = blurLoaded->properties.find(L"StandardDeviation");
            if (it != blurLoaded->properties.end()) {
                auto* fv = std::get_if<float>(&it->second);
                propOk = fv && std::abs(*fv - 3.0f) < 0.01f;
            }
        }
        TEST("PropertyPreserved", propOk);

        // Name lookups honour the view's length; a substring view is not null-terminated.
        const std::wstring_view longName = L"Gaussian Blur (suffix)";
        TEST("FindByNameUsesViewLength",
             ShaderLab::Effects::EffectRegistry::Instance().FindByName(longName.substr(0, 13)) != nullptr &&
             registry.FindByName(std::wstring_view(L"Gamut Source!").substr(0, 12)) != nullptr);

        // A malformed effectClsid is reported on the node, not loaded as GUID_NULL.
        std::wstring badJson(json);
        wchar_t blurClsid[64]{};
        StringFromGUID2(blurLoaded && blurLoaded->effectClsid ? *blurLoaded->effectClsid : GUID{}, blurClsid, 64);
        const auto clsidAt = badJson.find(blurClsid);
        if (clsidAt != std::wstring::npos)
            badJson.replace(clsidAt, wcslen(blurClsid), L"{not-a-clsid}");
        auto g3 = ShaderLab::Graph::GraphModel::FromJson(winrt::hstring(badJson));
        const auto* badNode = g3.FindNode(blurId);
        TEST("MalformedClsidReported", clsidAt != std::wstring::npos && badNode &&
             !badNode->effectClsid.has_value() && !badNode->runtimeError.empty());

        // Rendering keeps the load error rather than replacing it with a creation failure.
        {
            ShaderLab::Graph::EffectNode shaderNode;
            shaderNode.type = ShaderLab::Graph::NodeType::PixelShader;
            shaderNode.name = L"No CLSID";
            shaderNode.runtimeError = L"effectClsid '{bad-shader}' is not a valid CLSID.";
            const auto shaderId = g3.AddNode(std::move(shaderNode));
            ShaderLab::Graph::EffectNode bareNode;
            bareNode.type = ShaderLab::Graph::NodeType::BuiltInEffect;
            bareNode.name = L"Bare";
            const auto bareId = g3.AddNode(std::move(bareNode));

            ShaderLab::Rendering::GraphEvaluator evaluator;
            g3.MarkAllDirty();
            evaluator.Evaluate(g3, g_dc.get());
            evaluator.Evaluate(g3, g_dc.get());

            const auto* blurAfter = g3.FindNode(blurId);
            const auto* shaderAfter = g3.FindNode(shaderId);
            const auto* bareAfter = g3.FindNode(bareId);
            TEST("MalformedClsidSurvivesEvaluate", blurAfter &&
                 blurAfter->runtimeError.find(L"{not-a-clsid}") != std::wstring::npos);
            TEST("MalformedShaderClsidSurvivesEvaluate", shaderAfter &&
                 shaderAfter->runtimeError.find(L"{bad-shader}") != std::wstring::npos);
            TEST("MissingClsidHasReason", bareAfter &&
                 bareAfter->runtimeError.find(L"no effect CLSID") != std::wstring::npos);
        }
    }

    SL_TEST_CASE(TestPropertyBindings, Tag::Gpu)
    {
        printf("\n=== Property Bindings ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::GraphModel g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto paramNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Float Parameter"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto paramId = g.AddNode(std::move(paramNode));
        auto blurId = g.AddNode(std::move(blurNode));
        g.Connect(srcId, 0, blurId, 0);

        ShaderLab::Graph::SetNodeProperty(*g.FindNode(paramId), L"Value", 5.0f);
        auto err = g.BindProperty(blurId, L"StandardDeviation", paramId, L"Value", 0);
        TEST("BindProperty", err.empty());

        PrepareSourcesAndEvaluate(g, sf);
        auto* blur = g.FindNode(blurId);
        auto sdIt = blur->properties.find(L"StandardDeviation");
        bool sdOk = false;
        if (sdIt != blur->properties.end()) {
            auto* fv = std::get_if<float>(&sdIt->second);
            sdOk = fv && *fv >= 4.5f && *fv <= 5.5f;
        }
        TEST("BindingPropagates", sdOk);
    }

    // A binding cycle names the nodes it blocks instead of blanking the
    // frame silently; an image Source with no file yet is not an error.
    SL_TEST_CASE(TestContractsOnDevice, Tag::Gpu)
    {
        printf("\n=== Contracts on device ===\n");
        using namespace ShaderLab::Graph;
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        const auto* floatParam = registry.FindByName(L"Float Parameter");
        GraphModel g;
        const uint32_t a = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*floatParam));
        const uint32_t b = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*floatParam));
        TEST("Cycle_FirstBindingAccepted", g.BindProperty(a, L"Value", b, L"Value").empty());
        TEST("Cycle_ClosingBindingRefused", !g.BindProperty(b, L"Value", a, L"Value").empty());
        // A hand-edited file can still carry one.
        g.FindNode(b)->propertyBindings[L"Value"].sources = { ComponentSource{ a, L"Value", 0, 0 } };
        ShaderLab::Effects::SourceNodeFactory sf;
        PrepareSourcesAndEvaluate(g, sf);
        TEST("Cycle_NodesCarryError",
             g.FindNode(a)->runtimeError.find(L"cycle") != std::wstring::npos &&
             g.FindNode(b)->runtimeError.find(L"cycle") != std::wstring::npos);
        g.UnbindProperty(b, L"Value");
        PrepareSourcesAndEvaluate(g, sf);
        TEST("Cycle_ErrorClearsWhenBroken", g.FindNode(a)->runtimeError.empty() && g.FindNode(b)->runtimeError.empty());

        auto empty = ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"");
        empty.id = 1;
        sf.PrepareSourceNode(empty, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        TEST("Source_EmptyPathIsNoFileYet", empty.runtimeError.empty() && empty.cachedOutput == nullptr);
        empty.runtimeError = L"Failed to decode image: gone.png";
        sf.PrepareSourceNode(empty, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        TEST("Source_ClearedPathClearsDecodeError", empty.runtimeError.empty());
    }

    SL_TEST_CASE(TestClockNode, Tag::Fast)
    {
        printf("\n=== Clock Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto* desc = registry.FindByName(L"Clock");
        TEST("ClockExists", desc != nullptr);
        if (desc) {
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            TEST("IsClock", node.IsClock());
            TEST("HasAutoDuration", node.properties.count(L"AutoDuration") > 0);
            TEST("HasTimeOutput", !node.customEffect->analysisFields.empty() &&
                node.customEffect->analysisFields[0].name == L"Time");
        }
    }
}
