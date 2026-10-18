#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Graph/SourceKind.h"
#include "../../Effects/LiveSource.h"
#include "../../Effects/NodeCatalog.h"
#include "../../Effects/SourceNodeFactory.h"
#include "Graph/GraphEdits.h"

#include <cstdio>
#include <thread>

// The live-source hook a host plugs its own Sources into: the marker
// registry, the factory's open / tick / retry / prune cycle, targets, and
// host node makers in the catalog. A scripted fake stands in for a capture.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Effects;
        using ShaderLab::Graph::ClassifySource;
        using ShaderLab::Graph::EffectNode;
        using ShaderLab::Graph::SourceKind;

        constexpr const wchar_t* cFakeMarker = L"IsEffectGraphTestLive";

        // What the fake does next, and what happened to it. Shared by every
        // fake the test kind makes.
        struct FakeScript
        {
            bool openSucceeds{ true };
            bool reopenSucceeds{ true };
            CaptureResult next{ CaptureResult::NoFrame };
            std::wstring error{ L"scripted failure" };
            bool createFails{ false };

            int created{ 0 };
            int destroyed{ 0 };
            int opens{ 0 };
            int reopens{ 0 };
            std::wstring lastTarget;
        };
        FakeScript g_script;

        class FakeLiveSource final : public ILiveSource
        {
        public:
            ~FakeLiveSource() override { ++g_script.destroyed; }

            bool Open(const EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device*) override
            {
                ++g_script.opens;
                m_key = KeyOf(node);
                return Start(dc, g_script.openSucceeds);
            }

            bool Reopen(ID2D1DeviceContext5* dc) override
            {
                ++g_script.reopens;
                return Start(dc, g_script.reopenSucceeds);
            }

            bool Matches(const EffectNode& node) const override { return KeyOf(node) == m_key; }
            bool IsOpen() const override { return m_image != nullptr; }

            CaptureResult NextFrame(ID2D1DeviceContext5*) override
            {
                if (g_script.next == CaptureResult::Failed)
                {
                    m_error = g_script.error;
                    m_image = nullptr;
                }
                else if (g_script.next == CaptureResult::NewFrame)
                {
                    ++m_frames;
                }
                return g_script.next;
            }

            ID2D1Image* CurrentImage() const override { return m_image.get(); }
            uint32_t Width() const override { return 4; }
            uint32_t Height() const override { return 2; }
            uint64_t FrameCount() const override { return m_frames; }
            bool DeliversFP16() const override { return true; }
            const std::wstring& LastError() const override { return m_error; }
            void Describe(EffectNode& node) const override { node.name = L"Fake opened"; }

        private:
            static uint32_t KeyOf(const EffectNode& node)
            {
                auto it = node.properties.find(L"Key");
                if (it == node.properties.end()) return 0;
                auto* key = std::get_if<uint32_t>(&it->second);
                return key ? *key : 0;
            }

            bool Start(ID2D1DeviceContext5* dc, bool succeeds)
            {
                m_image = nullptr;
                if (!succeeds)
                {
                    m_error = g_script.error;
                    return false;
                }
                winrt::com_ptr<ID2D1Effect> flood;
                if (FAILED(dc->CreateEffect(CLSID_D2D1Flood, flood.put())))
                {
                    m_error = L"no flood";
                    return false;
                }
                flood->GetOutput(m_image.put());
                m_flood = flood;
                m_error.clear();
                return true;
            }

            uint32_t m_key{ 0 };
            uint64_t m_frames{ 0 };
            winrt::com_ptr<ID2D1Effect> m_flood;
            winrt::com_ptr<ID2D1Image> m_image;
            std::wstring m_error;
        };

        void RegisterFakeKind()
        {
            LiveSourceKind kind;
            kind.marker = cFakeMarker;
            kind.displayName = L"test live source";
            kind.create = [](winrt::Windows::Foundation::IInspectable const& target, std::wstring& error)
                -> std::unique_ptr<ILiveSource>
            {
                g_script.lastTarget = target ? std::wstring(winrt::unbox_value<winrt::hstring>(target)) : L"";
                if (g_script.createFails)
                {
                    error = L"Pick something first.";
                    return nullptr;
                }
                ++g_script.created;
                return std::make_unique<FakeLiveSource>();
            };
            RegisterLiveSourceKind(std::move(kind));
        }

        float FieldValue(const EffectNode& node, std::wstring_view name)
        {
            for (const auto& field : node.analysisOutput.fields)
                if (field.name == name) return field.components[0];
            return -1.0f;
        }

        SL_TEST_CASE(TestLiveSourceMarkers, Tag::Fast)
        {
            std::printf("\n=== Live source markers ===\n");
            EffectNode node = SourceNodeFactory::CreateLiveSourceNode(L"IsEffectGraphTestUnregistered", L"Nobody's");
            TEST("LiveMarker_UnregisteredIsNone", ClassifySource(node) == SourceKind::None);

            RegisterFakeKind();
            EffectNode live = SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Fake");
            TEST("LiveMarker_RegisteredIsLive", ClassifySource(live) == SourceKind::Live && FindLiveSourceKind(live) != nullptr);
            ShaderLab::Graph::SetNodeProperty(live, cFakeMarker, false);
            TEST("LiveMarker_FalseMarkerIgnored", ClassifySource(live) == SourceKind::None);

            const EffectNode seeded = SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Fake");
            bool fieldsSeeded = seeded.analysisOutput.fields.size() == SourceNodeFactory::scLiveFields.size();
            for (size_t i = 0; fieldsSeeded && i < SourceNodeFactory::scLiveFields.size(); ++i)
                fieldsSeeded = seeded.analysisOutput.fields[i].name == SourceNodeFactory::scLiveFields[i];
            TEST("LiveMarker_FieldsSeededAtCreation", fieldsSeeded && seeded.outputPins.size() == 1);
        }

        SL_TEST_CASE(TestLiveSourceLifecycle, Tag::Gpu)
        {
            std::printf("\n=== Live source open, tick, retry, prune ===\n");
            RegisterFakeKind();
            g_script = {};
            ID2D1DeviceContext5* dc = g_dc.get();
            ID3D11Device* device = g_d3dDevice.get();

            Graph::GraphModel graph;
            SourceNodeFactory factory;
            const uint32_t id = graph.AddNode(SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Fake"));
            auto node = [&]() -> EffectNode& { return *graph.FindNode(id); };
            auto tick = [&] { return factory.TickLiveSources(graph.MutableNodes(), dc); };

            // Open: the target reaches the kind, the node points at the image.
            factory.SetLiveSourceTarget(id, winrt::box_value(L"window 7"));
            factory.PrepareSourceNode(node(), dc, device);
            TEST("Live_OpensWithTarget", g_script.opens == 1 && g_script.lastTarget == L"window 7" &&
                 node().cachedOutput != nullptr && node().runtimeError.empty() && node().name == L"Fake opened");

            // A new frame dirties the node and fills the fields.
            node().dirty = false;
            g_script.next = CaptureResult::NewFrame;
            const bool newFrame = tick();
            TEST("Live_NewFrameDirtiesNode", newFrame && node().dirty &&
                 FieldValue(node(), L"Width") == 4.0f && FieldValue(node(), L"Frames") == 1.0f &&
                 FieldValue(node(), L"FP16") == 1.0f);

            // No new frame: the node is left clean.
            node().dirty = false;
            g_script.next = CaptureResult::NoFrame;
            TEST("Live_NoFrameLeavesNodeClean", !tick() && !node().dirty);

            // Lost: no output, and the reason on the node.
            g_script.next = CaptureResult::Failed;
            g_script.error = L"mode change";
            tick();
            std::printf("  [info] lost: '%ls'\n", node().runtimeError.c_str());
            TEST("Live_LostSaysWhy", node().cachedOutput == nullptr &&
                 node().runtimeError == L"Live source lost, retrying: mode change");

            // Reopened once the retry interval passes; the error clears.
            g_script.next = CaptureResult::NoFrame;
            tick();
            const bool notBeforeInterval = g_script.reopens == 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            tick();
            TEST("Live_ReopensAfterInterval", notBeforeInterval && g_script.reopens == 1 &&
                 node().cachedOutput != nullptr && node().runtimeError.empty());

            // Properties that no longer match make a new source.
            ShaderLab::Graph::SetNodeProperty(node(), L"Key", uint32_t{ 2 });
            factory.PrepareSourceNode(node(), dc, device);
            TEST("Live_MismatchRecreates", g_script.created == 2 && g_script.destroyed == 1 && g_script.opens == 2);

            // Releasing device objects keeps the target.
            factory.ReleaseCache();
            g_script.lastTarget.clear();
            factory.PrepareSourceNode(node(), dc, device);
            TEST("Live_TargetSurvivesRelease", g_script.lastTarget == L"window 7" && node().cachedOutput != nullptr);

            // A removed node's source is destroyed on the next tick.
            const int destroyedBefore = g_script.destroyed;
            graph.RemoveNode(id);
            tick();
            TEST("Live_RemovedNodeDropsSource", g_script.destroyed == destroyedBefore + 1);
        }

        SL_TEST_CASE(TestLiveSourceFailures, Tag::Gpu)
        {
            std::printf("\n=== Live source failures ===\n");
            RegisterFakeKind();
            g_script = {};
            ID2D1DeviceContext5* dc = g_dc.get();
            ID3D11Device* device = g_d3dDevice.get();

            Graph::GraphModel graph;
            SourceNodeFactory factory;

            // The kind declines to make a source: its message, no retry.
            g_script.createFails = true;
            const uint32_t declined = graph.AddNode(SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Fake"));
            factory.PrepareSourceNode(*graph.FindNode(declined), dc, device);
            TEST("LiveFail_CreateDeclinedSaysWhy", graph.FindNode(declined)->runtimeError == L"Pick something first." &&
                 graph.FindNode(declined)->cachedOutput == nullptr);
            graph.RemoveNode(declined);

            // An open failure names the kind and keeps that message, with the
            // latest reason, until a reopen succeeds.
            g_script = {};
            g_script.openSucceeds = false;
            g_script.reopenSucceeds = false;
            g_script.error = L"output gone";
            const uint32_t id = graph.AddNode(SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Fake"));
            factory.PrepareSourceNode(*graph.FindNode(id), dc, device);
            const std::wstring openError = graph.FindNode(id)->runtimeError;
            factory.TickLiveSources(graph.MutableNodes(), dc);
            g_script.error = L"still gone";
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            factory.TickLiveSources(graph.MutableNodes(), dc);
            std::printf("  [info] open: '%ls', after retry: '%ls'\n", openError.c_str(),
                        graph.FindNode(id)->runtimeError.c_str());
            TEST("LiveFail_OpenFailureNamesKind", openError == L"Failed to open test live source: output gone");
            TEST("LiveFail_RetryRefreshesReason", g_script.reopens == 1 &&
                 graph.FindNode(id)->runtimeError == L"Failed to open test live source: still gone");

            g_script.reopenSucceeds = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            factory.TickLiveSources(graph.MutableNodes(), dc);
            TEST("LiveFail_RecoveredOpenClearsAndDescribes", graph.FindNode(id)->runtimeError.empty() &&
                 graph.FindNode(id)->cachedOutput != nullptr && graph.FindNode(id)->name == L"Fake opened");
        }

        SL_TEST_CASE(TestNodeMakers, Tag::Fast)
        {
            std::printf("\n=== Host node makers ===\n");
            RegisterFakeKind();
            RegisterNodeMaker({ L"Test Live", L"Output" }, [](const NodeCreateArgs& args, const Graph::GraphModel&)
            {
                NodeCreateResult result;
                auto key = args.uints.find(L"key");
                if (key == args.uints.end())
                {
                    result.error = L"Test Live needs a key.";
                    return result;
                }
                result.node = SourceNodeFactory::CreateLiveSourceNode(cFakeMarker, L"Test Live");
                result.node->properties[L"Key"] = key->second;
                result.needsPrepare = true;
                return result;
            });

            Graph::GraphModel graph;
            NodeCreateArgs args;
            args.uints[L"key"] = 9;
            auto made = CreateNodeByName(L"Test Live", args, graph);
            TEST("NodeMaker_Makes", made.node && made.needsPrepare && ClassifySource(*made.node) == SourceKind::Live &&
                 std::get<uint32_t>(made.node->properties.at(L"Key")) == 9);
            auto refused = CreateNodeByName(L"Test Live", {}, graph);
            TEST("NodeMaker_ErrorPassesThrough", !refused.node && refused.error == L"Test Live needs a key.");
            auto output = CreateNodeByName(L"Output", {}, graph);
            TEST("NodeMaker_BuiltInWins", output.node && output.node->type == Graph::NodeType::Output);
        }
    }
}
