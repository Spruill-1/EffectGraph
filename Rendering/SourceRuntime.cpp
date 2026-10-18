#include "pch_engine.h"
#include "SourceRuntime.h"
#include "GraphEvaluator.h"
#include "../Effects/SourceNodeFactory.h"
#include "../Graph/SourceKind.h"

namespace ShaderLab::Rendering
{
    using Graph::EffectNode;
    using Graph::NodeType;

    namespace
    {
        bool IsVideoNode(const EffectNode& node)
        {
            return Graph::ClassifySource(node) == Graph::SourceKind::Video;
        }

        // True when the uploaded frame is the one shown at `target`: the frame
        // whose [start, start + duration) contains it. The first frame also
        // covers any time before it, and the last any gap to the end.
        bool VideoShowsTime(const Effects::VideoSourceProvider& provider, double target)
        {
            const double frameStart = provider.UploadedFrameTime();
            if (frameStart < 0.0) return false;
            target = (std::max)(target, provider.FirstFrameTime());
            constexpr double cTolerance = 1e-4;
            double frameEnd = frameStart + (std::max)(provider.UploadedFrameDuration(), cTolerance);
            const double nominalFrame = 1.0 / (std::max)(provider.FrameRate(), 1.0);
            if (frameEnd + nominalFrame > provider.Duration()) frameEnd = (std::max)(frameEnd, provider.Duration());
            return target >= frameStart - cTolerance && target < frameEnd + cTolerance;
        }
    }

    void PrepareSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory,
                        const SourceDevices& devices, SourceScope scope)
    {
        for (auto& node : graph.MutableNodes())
        {
            if (node.type != NodeType::Source) continue;
            if (scope == SourceScope::Changed && !node.dirty && !factory.HasVideoProvider(node.id))
                continue;
            factory.PrepareSourceNode(node, devices.dc, devices.d3dDevice, devices.d3dContext);
        }
    }

    bool SettleVideoSources(Graph::GraphModel& graph, Effects::SourceNodeFactory& factory,
                            GraphEvaluator& evaluator, ID2D1DeviceContext5* dc,
                            std::chrono::milliseconds timeout)
    {
        auto nodes = graph.MutableNodes();
        const bool anyBoundTime = std::any_of(nodes.begin(), nodes.end(), [](const EffectNode& node)
            { return IsVideoNode(node) && node.propertyBindings.count(L"Time"); });
        if (anyBoundTime)
        {
            // Upstream values (a Clock's Time) only exist after evaluation.
            std::vector<uint32_t> dirtyIds;
            for (const auto& node : nodes)
                if (node.dirty) dirtyIds.push_back(node.id);
            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(nullptr);
            evaluator.Evaluate(graph, dc, GraphEvaluator::EvalMode::Frozen);
            dc->SetTarget(oldTarget.get());
            evaluator.ResolveSourceBindings(graph);
            for (uint32_t id : dirtyIds)
                if (auto* node = graph.FindNode(id)) node->dirty = true;
        }

        struct Pending
        {
            uint32_t nodeId;
            std::wstring name;
            std::wstring path;
            double target;
        };
        auto collectPending = [&]
        {
            std::vector<Pending> pending;
            for (const auto& node : nodes)
            {
                if (!IsVideoNode(node)) continue;
                auto* provider = factory.VideoProviderForRenderThread(node.id);
                if (!provider || !provider->IsOpen()) continue;
                const auto target = Effects::SourceNodeFactory::VideoTargetTime(node, provider->Duration());
                if (!target || VideoShowsTime(*provider, *target)) continue;
                const std::wstring path = node.mediaPath.value_or(L"");
                if (factory.VideoSettleTimedOut(node.id, path, *target)) continue;
                pending.push_back({ node.id, node.name, path, *target });
            }
            return pending;
        };

        constexpr auto cPoll = std::chrono::milliseconds(10);
        factory.TickAndUploadVideos(nodes, dc);
        auto pending = collectPending();
        // A seek decodes from the last keyframe, so give it real time.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pending.empty() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(cPoll);
            factory.TickAndUploadVideos(nodes, dc);
            pending = collectPending();
        }

        for (const auto& video : pending)
        {
            factory.RecordVideoSettleTimeout(video.nodeId, video.path, video.target);
            auto* provider = factory.VideoProviderForRenderThread(video.nodeId);
            std::fwprintf(stderr, L"[ShaderLab] video '%ls' did not deliver the frame for %.3f s within %.1f s "
                                  L"(frame on screen %.3f s); this run shows a different frame\n",
                          video.name.c_str(), video.target, timeout.count() / 1000.0,
                          provider ? provider->UploadedFrameTime() : -1.0);
        }
        return pending.empty();
    }
}
