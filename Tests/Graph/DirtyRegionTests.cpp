#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/DirtyRegion.h"
#include "../../Graph/GraphEdits.h"
#include "../../Graph/GraphModel.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceNodeFactory.h"

#include <cstdio>

// Dirty regions in the graph model: the region type, how a region maps from
// node to node (MapRegion), and how propagation carries it. No device.
namespace ShaderLab::Tests
{
    namespace
    {
        using Graph::DirtyRegion;

        Graph::EffectNode ShaderLabNode(const wchar_t* name)
        {
            const auto* desc = Effects::ShaderLabEffects::Instance().FindByName(name);
            return desc ? Effects::ShaderLabEffects::CreateNode(*desc) : Graph::EffectNode{};
        }

        Graph::EffectNode D2DNode(const wchar_t* name)
        {
            const auto* desc = Effects::EffectRegistry::Instance().FindByName(name);
            return desc ? Effects::EffectRegistry::CreateNode(*desc) : Graph::EffectNode{};
        }

        Graph::EffectNode InputNode()
        {
            return Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Texture Input");
        }

        bool SameRect(const D2D1_RECT_L& a, const D2D1_RECT_L& b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        // The region holds exactly `rects`, in any order.
        bool Holds(const DirtyRegion& region, std::initializer_list<D2D1_RECT_L> rects)
        {
            if (!region.IsRects() || region.Rects().size() != rects.size()) return false;
            for (const auto& rect : rects)
                if (std::ranges::none_of(region.Rects(), [&](const Graph::DirtyRect& held) { return SameRect(held.rect, rect); }))
                    return false;
            return true;
        }

        void CleanAll(Graph::GraphModel& graph)
        {
            for (auto& node : graph.MutableNodes()) node.ClearDirty();
        }

        const DirtyRegion& RegionOf(Graph::GraphModel& graph, uint32_t id)
        {
            return graph.FindNode(id)->Region();
        }
    }

    SL_TEST_CASE(TestDirtyRegionType, Tag::Fast)
    {
        printf("\n=== Dirty region: the type ===\n");
        DirtyRegion region;
        TEST("Region_StartsNone", region.IsNone() && region.Rects().empty());
        region.Add({ 5, 5, 5, 9 }, 1);
        TEST("Region_EmptyRectIgnored", region.IsNone());
        region.Add({ 0, 0, 10, 10 }, 3);
        region.Add({ 2, 2, 4, 4 }, 1);
        TEST("Region_ContainedRectAbsorbed", Holds(region, { { 0, 0, 10, 10 } }) && region.Rects()[0].generation == 1);
        region.Add({ 20, 0, 30, 10 }, 4);
        region.Add({ 0, 0, 40, 10 }, 6);
        TEST("Region_ContainingRectReplaces", Holds(region, { { 0, 0, 40, 10 } }) && region.Rects()[0].generation == 1);

        DirtyRegion other = DirtyRegion::Rect({ 50, 50, 60, 60 }, 2);
        region.Union(other);
        TEST("Region_UnionKeepsBoth", Holds(region, { { 0, 0, 40, 10 }, { 50, 50, 60, 60 } }));
        DirtyRegion whole = region;
        whole.Union(DirtyRegion::Whole());
        TEST("Region_WholeAbsorbs", whole.IsWhole() && whole.Rects().empty());
        whole.Add({ 0, 0, 1, 1 }, 9);
        TEST("Region_WholeStaysWhole", whole.IsWhole());

        // Clipping, area, cover.
        const DirtyRegion clipped = region.ClippedTo({ 0, 0, 32, 55 });
        TEST("Region_ClippedDropsEmpties", Holds(clipped, { { 0, 0, 32, 10 } }));
        DirtyRegion halves = DirtyRegion::Rect({ 0, 0, 8, 4 }, 1);
        halves.Add({ 4, 0, 16, 4 }, 1);
        halves.Add({ 0, 4, 16, 8 }, 1);
        TEST("Region_UnionAreaCountsOverlapOnce", halves.UnionArea() == 128);
        TEST("Region_CoversExactly", halves.Covers({ 0, 0, 16, 8 }) && !halves.Covers({ 0, 0, 17, 8 }));

        // A 17th rect collapses all of them to their bounding box, tagged
        // with the oldest rect's generation.
        DirtyRegion many;
        for (LONG i = 0; i < 16; ++i)
            many.Add({ i * 10, i * 3, i * 10 + 4, i * 3 + 2 }, 20 - static_cast<uint64_t>(i == 7 ? 15 : 0));
        const bool sixteenKept = many.IsRects() && many.Rects().size() == 16;
        many.Add({ 500, 400, 502, 402 }, 30);
        printf("  [info] 16 rects kept: %d; after the 17th: %zu rect(s), generation %llu\n", sixteenKept,
               many.Rects().size(), static_cast<unsigned long long>(many.OldestGeneration()));
        TEST("Region_SixteenRectsKept", sixteenKept);
        TEST("Region_SeventeenthCollapsesToBoundingBox", Holds(many, { { 0, 0, 502, 402 } }));
        TEST("Region_CollapseKeepsOldestTag", many.Rects().size() == 1 && many.Rects()[0].generation == 5);
    }

    SL_TEST_CASE(TestDirtyRegionPropagation, Tag::Fast)
    {
        printf("\n=== Dirty region: mapping and propagation ===\n");
        Graph::GraphModel graph;
        const uint32_t input = graph.AddNode(InputNode());
        const uint32_t shader = graph.AddNode(ShaderLabNode(L"Nit Map"));
        const uint32_t output = graph.AddNode(Effects::EffectRegistry::CreateOutputNode());
        const uint32_t blur = graph.AddNode(D2DNode(L"Gaussian Blur"));
        const uint32_t afterBlur = graph.AddNode(ShaderLabNode(L"Nit Map"));
        TEST("Propagate_NodesMade", graph.FindNode(shader)->customEffect.has_value() &&
                                    graph.FindNode(blur)->effectClsid.has_value());
        graph.Connect(input, 0, shader, 0);
        graph.Connect(shader, 0, output, 0);
        graph.Connect(input, 0, blur, 0);
        graph.Connect(blur, 0, afterBlur, 0);

        // Identity through a custom pixel shader and the Output.
        CleanAll(graph);
        const D2D1_RECT_L rect{ 10, 20, 30, 40 };
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 7));
        const DirtyRegion previewed = graph.PendingDirtyRegion(output);
        TEST("Propagate_PreviewChangesNothing", Holds(previewed, { rect }) && !graph.FindNode(shader)->IsDirty() &&
                                                !graph.FindNode(output)->IsDirty());
        graph.PropagateDirtyDownstream();
        TEST("Propagate_IdentityThroughCustomShader", Holds(RegionOf(graph, shader), { rect }) &&
                                                      RegionOf(graph, shader).Rects()[0].generation == 7);
        TEST("Propagate_IdentityThroughOutput", Holds(RegionOf(graph, output), { rect }));
        // Whole through a D2D built-in, and everything after it.
        TEST("Propagate_WholeThroughBlur", RegionOf(graph, blur).IsWhole() && RegionOf(graph, afterBlur).IsWhole());

        // A property edit on a region-dirty node makes it whole.
        Graph::SetNodeProperty(graph, shader, L"MaxNits", 400.0f);
        TEST("Propagate_PropertyEditMakesWhole", RegionOf(graph, shader).IsWhole());
        // And a raw MarkDirty is whole: no writer can leave a node in part.
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
        graph.FindNode(input)->MarkDirty();
        TEST("Propagate_MarkDirtyIsWhole", RegionOf(graph, input).IsWhole());

        // Whole into a lookup pin; the same rects into the image pin beside it.
        auto lookupNode = ShaderLabNode(L"Nit Map");
        lookupNode.customEffect->inputNames = { L"Source", L"Table" };
        lookupNode.customEffect->lookupInputCount = 1;
        lookupNode.inputPins = { { L"Source", 0 }, { L"Table", 1 } };
        const uint32_t lookup = graph.AddNode(lookupNode);
        const uint32_t table = graph.AddNode(InputNode());
        graph.Connect(input, 0, lookup, 0);
        graph.Connect(table, 0, lookup, 1);
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
        graph.PropagateDirtyDownstream();
        TEST("Propagate_ImagePinBesideLookupKeepsRects", Holds(RegionOf(graph, lookup), { rect }));
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, table, DirtyRegion::Rect(rect, 1));
        graph.PropagateDirtyDownstream();
        TEST("Propagate_WholeThroughLookupPin", RegionOf(graph, lookup).IsWhole());

        // A diagram (fixed output extent) is not laid out in its input's pixels.
        const uint32_t diagram = graph.AddNode(ShaderLabNode(L"ICtCp Boundary"));
        graph.Connect(input, 0, diagram, 0);
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
        graph.PropagateDirtyDownstream();
        TEST("Propagate_WholeIntoDiagram", graph.FindNode(diagram)->customEffect.has_value() &&
                                           RegionOf(graph, diagram).IsWhole());

        // Into a D3D11 compute node the rects are its input region; what it
        // emits (here through an image-output compute node) is whole.
        const uint32_t stats = graph.AddNode(ShaderLabNode(L"Luminance Statistics"));
        const uint32_t computeMap = graph.AddNode(ShaderLabNode(L"ICtCp Tone Map (HDR -> SDR)"));
        const uint32_t afterCompute = graph.AddNode(ShaderLabNode(L"Nit Map"));
        graph.Connect(input, 0, stats, 0);
        graph.Connect(input, 0, computeMap, 0);
        graph.Connect(computeMap, 0, afterCompute, 0);
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
        const DirtyRegion computeEmits = graph.PendingDirtyRegion(computeMap);
        graph.PropagateDirtyDownstream();
        TEST("Propagate_ComputeInputRegionKeepsRects", Holds(RegionOf(graph, stats), { rect }) &&
                                                       Holds(RegionOf(graph, computeMap), { rect }));
        TEST("Propagate_WholeOutOfCompute", graph.FindNode(computeMap)->customEffect.has_value() &&
                                            RegionOf(graph, afterCompute).IsWhole() && computeEmits.IsWhole());

        // The switch off: every region whole.
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
        graph.PropagateDirtyDownstream(false);
        TEST("Propagate_SwitchOffIsWhole", RegionOf(graph, input).IsWhole() && RegionOf(graph, shader).IsWhole() &&
                                           RegionOf(graph, output).IsWhole());
    }

    SL_TEST_CASE(TestDirtyRegionTwoPaths, Tag::Fast)
    {
        printf("\n=== Dirty region: two paths into one node ===\n");
        // Two inputs, each through its own per-pixel shader, into the two
        // image pins of Split Comparison: both paths' rects must arrive. A
        // walk that stops at a node it has visited keeps only the first.
        Graph::GraphModel graph;
        const uint32_t a = graph.AddNode(InputNode());
        const uint32_t b = graph.AddNode(InputNode());
        const uint32_t shaderA = graph.AddNode(ShaderLabNode(L"Nit Map"));
        const uint32_t shaderB = graph.AddNode(ShaderLabNode(L"Nit Map"));
        const uint32_t split = graph.AddNode(ShaderLabNode(L"Split Comparison"));
        const uint32_t output = graph.AddNode(Effects::EffectRegistry::CreateOutputNode());
        graph.Connect(a, 0, shaderA, 0);
        graph.Connect(b, 0, shaderB, 0);
        graph.Connect(shaderA, 0, split, 0);
        graph.Connect(shaderB, 0, split, 1);
        graph.Connect(split, 0, output, 0);
        TEST("TwoPaths_Wired", graph.GetInputEdges(split).size() == 2);

        CleanAll(graph);
        const D2D1_RECT_L ra{ 0, 0, 8, 8 }, rb{ 100, 100, 120, 110 };
        Graph::MarkNodeChanged(graph, a, DirtyRegion::Rect(ra, 1));
        Graph::MarkNodeChanged(graph, b, DirtyRegion::Rect(rb, 2));
        graph.PropagateDirtyDownstream();
        TEST("TwoPaths_BothSurvive", Holds(RegionOf(graph, split), { ra, rb }) && Holds(RegionOf(graph, output), { ra, rb }));

        // The same when one path's node is already dirty in part.
        CleanAll(graph);
        Graph::MarkNodeChanged(graph, shaderA, DirtyRegion::Rect(ra, 1));
        Graph::MarkNodeChanged(graph, b, DirtyRegion::Rect(rb, 2));
        graph.PropagateDirtyDownstream();
        TEST("TwoPaths_StartNodeUnions", Holds(RegionOf(graph, split), { ra, rb }) && !graph.FindNode(a)->IsDirty());

        // A clean start node (MarkDownstreamDirty) dirties what it feeds, whole, and stays clean.
        CleanAll(graph);
        graph.MarkDownstreamDirty(a);
        TEST("TwoPaths_DownstreamOfCleanNodeWhole", !graph.FindNode(a)->IsDirty() && RegionOf(graph, shaderA).IsWhole() &&
                                                    RegionOf(graph, output).IsWhole() && !graph.FindNode(b)->IsDirty());
    }

    // The replay chain's effects keep rects: the built-in Gamut Map, and the
    // plugin's tone map when the runner loaded one. Were either Whole, every
    // replay present would draw the whole target and the replay's clip and
    // equality checks would hold without the rect path running.
    SL_TEST_CASE(TestDirtyRegionChainEffects, Tag::Fast)
    {
        printf("\n=== Dirty region: the replay chain's effects ===\n");
        std::vector<std::wstring> names{ L"Gamut Map" };
        if (!g_pluginPath.empty()) names.push_back(L"HDR Screenshot Tonemap (8bpc)");
        for (const auto& name : names)
        {
            Graph::GraphModel graph;
            const uint32_t input = graph.AddNode(InputNode());
            const uint32_t chain = graph.AddNode(ShaderLabNode(name.c_str()));
            graph.Connect(input, 0, chain, 0);
            CleanAll(graph);
            const D2D1_RECT_L rect{ 10, 20, 30, 40 };
            Graph::MarkNodeChanged(graph, input, DirtyRegion::Rect(rect, 1));
            graph.PropagateDirtyDownstream();
            std::string label;
            for (wchar_t c : name) label += (c == L' ' || c == L'(' || c == L')') ? '_' : static_cast<char>(c);
            TEST(("Chain_KeepsRects_" + label).c_str(), graph.FindNode(chain)->customEffect.has_value() &&
                                                        Holds(RegionOf(graph, chain), { rect }));
        }
        if (g_pluginPath.empty())
            ReportNotRun("Chain_KeepsRects for the plugin tone map: needs --plugin <PrivateEffects.dll>");
    }

    // Moves: the region type, the draw-time plan, and which nodes carry them.
    SL_TEST_CASE(TestDirtyRegionMoves, Tag::Fast)
    {
        printf("\n=== Dirty region: moves ===\n");
        using Graph::DirtyMove;
        const DirtyMove m1{ { 40, 0, 80, 40 }, { 0, 0 }, 5, 11 };
        const DirtyMove m2{ { 100, 40, 140, 80 }, { 40, 0 }, 5, 12 };

        DirtyRegion region;
        region.AddMove(m1);
        region.AddMove(m1);   // the same sequence, reached twice
        TEST("Moves_RectsWithOnlyMoves", region.IsRects() && region.Rects().empty() && region.Moves().size() == 1);
        DirtyRegion other;
        other.AddMove(m2);
        other.AddMove(m1);
        region.Union(other);
        TEST("Moves_UnionDedupesInOrder", region.Moves().size() == 2 && region.Moves()[0] == m1 && region.Moves()[1] == m2);
        TEST("Moves_EqualityComparesMoves", !(region == DirtyRegion::Rect({ 0, 0, 1, 1 }, 5)) && region == region);

        auto collapsed = region.WithMovesCollapsed();
        TEST("Moves_CollapseToOlderRects", !collapsed.HasMoves() && Holds(collapsed, { m1.destination, m2.destination }) &&
                                          collapsed.OldestGeneration() == 4);
        auto whole = region;
        whole.Union(DirtyRegion::Whole());
        TEST("Moves_WholeDropsThem", whole.IsWhole() && !whole.HasMoves());
        DirtyRegion many;
        for (uint64_t i = 0; i <= DirtyRegion::cMaxMoves; ++i)
            many.AddMove({ { static_cast<LONG>(i * 4), 0, static_cast<LONG>(i * 4 + 2), 2 }, { 0, 10 }, 5, 100 + i });
        TEST("Moves_PastCapacityCollapse", !many.HasMoves() && many.IsRects() && !many.Rects().empty());
        const auto clipped = region.ClippedTo({ 0, 0, 120, 120 });
        TEST("Moves_ClippedOutsideBecomesRect", clipped.Moves().size() == 1 && clipped.Moves()[0] == m1 &&
                                                Holds(clipped, { D2D1_RECT_L{ 100, 40, 120, 80 } }));

        // The plan: kept in order; a source an older rect overlaps collapses,
        // a same-generation rect does not; a carry-over collapses; so does a
        // listed sequence; and a later move reading a collapsed destination.
        auto plan = Graph::PlanMoves(region, 4, true);
        TEST("Plan_KeepsInOrder", plan.kept.size() == 2 && plan.kept[0] == m1 && plan.kept[1] == m2 && plan.collapsed == 0 &&
                                  plan.rects.IsNone());
        auto withRect = region;
        withRect.Add({ 0, 0, 10, 10 }, 5);
        plan = Graph::PlanMoves(withRect, 4, true);
        TEST("Plan_SameGenerationRectKeeps", plan.kept.size() == 2 && Holds(plan.rects, { D2D1_RECT_L{ 0, 0, 10, 10 } }));
        auto stale = region;
        stale.Add({ 0, 0, 10, 10 }, 4);
        plan = Graph::PlanMoves(stale, 4, true);
        TEST("Plan_OlderRectCollapsesChain", plan.kept.empty() && plan.collapsed == 2 &&
                                             Holds(plan.rects, { D2D1_RECT_L{ 0, 0, 10, 10 }, m1.destination, m2.destination }));
        plan = Graph::PlanMoves(region, 5, true);
        TEST("Plan_CarryOverCollapses", plan.kept.empty() && plan.collapsed == 2);
        // An older rect collapses only the moves whose source it overlaps.
        DirtyRegion apartStale;
        apartStale.Add({ 0, 0, 10, 10 }, 4);
        apartStale.AddMove(m1);
        apartStale.AddMove({ { 100, 40, 140, 80 }, { 0, 60 }, 5, 13 });
        plan = Graph::PlanMoves(apartStale, 4, true);
        TEST("Plan_OlderRectCollapsesOnlyWhatItOverlaps", plan.kept.size() == 1 && plan.kept[0].sequence == 13 &&
                                                          plan.collapsed == 1 &&
                                                          Holds(plan.rects, { D2D1_RECT_L{ 0, 0, 10, 10 }, m1.destination }));
        // Ingest order across a MoveRects that advanced the generation: a
        // rect of the generation before is older than the move, a rect of
        // its own is not.
        DirtyRegion folded;
        folded.Add({ 60, 0, 70, 10 }, 5);
        folded.AddMove({ { 100, 40, 140, 80 }, { 60, 0 }, 6, 14 });
        plan = Graph::PlanMoves(folded, 4, true);
        TEST("Plan_EarlierFrameRectCollapses", plan.kept.empty() && plan.collapsed == 1);
        DirtyRegion sameFrame;
        sameFrame.Add({ 60, 0, 70, 10 }, 6);
        sameFrame.AddMove({ { 100, 40, 140, 80 }, { 60, 0 }, 6, 14 });
        plan = Graph::PlanMoves(sameFrame, 4, true);
        TEST("Plan_SameFrameRectKeeps", plan.kept.size() == 1 && plan.collapsed == 0);
        const uint64_t listed[] = { 11 };
        plan = Graph::PlanMoves(region, 4, true, listed);
        TEST("Plan_ListedCollapsesAndChains", plan.kept.empty() && plan.collapsed == 2);
        DirtyRegion apart;
        apart.AddMove(m1);
        apart.AddMove({ { 100, 40, 140, 80 }, { 0, 60 }, 5, 13 });
        plan = Graph::PlanMoves(apart, 4, true, listed);
        TEST("Plan_ListedLeavesOthers", plan.kept.size() == 1 && plan.kept[0].sequence == 13 && plan.collapsed == 1);
        plan = Graph::PlanMoves(region, 4, false);
        TEST("Plan_ApplyOffCollapsesAll", plan.kept.empty() && plan.collapsed == 2 &&
                                          Holds(plan.rects, { m1.destination, m2.destination }));

        // MapRegion: kept into Output and a shift-invariant one-image-pin
        // shader; collapsed into one that is not, and into a compute node.
        Graph::GraphModel graph;
        const uint32_t input = graph.AddNode(InputNode());
        const uint32_t invariant = graph.AddNode(ShaderLabNode(L"Gamut Map"));
        const uint32_t plain = graph.AddNode(ShaderLabNode(L"Nit Map"));
        const uint32_t output = graph.AddNode(Effects::EffectRegistry::CreateOutputNode());
        const uint32_t stats = graph.AddNode(ShaderLabNode(L"Luminance Statistics"));
        graph.FindNode(invariant)->customEffect->shiftInvariant = true;
        graph.Connect(input, 0, invariant, 0);
        graph.Connect(input, 0, plain, 0);
        graph.Connect(invariant, 0, output, 0);
        graph.Connect(input, 0, stats, 0);
        CleanAll(graph);
        DirtyRegion moved;
        moved.AddMove(m1);
        Graph::MarkNodeChanged(graph, input, moved);
        graph.PropagateDirtyDownstream();
        TEST("Map_ShiftInvariantKeepsMoves", RegionOf(graph, invariant).Moves().size() == 1 &&
                                             RegionOf(graph, invariant).Rects().empty());
        TEST("Map_OutputKeepsMoves", RegionOf(graph, output).Moves().size() == 1);
        TEST("Map_NotShiftInvariantCollapses", !RegionOf(graph, plain).HasMoves() &&
                                               Holds(RegionOf(graph, plain), { m1.destination }));
        TEST("Map_ComputeCollapses", !RegionOf(graph, stats).HasMoves() && Holds(RegionOf(graph, stats), { m1.destination }));

        // A shift-invariant shader with a second image pin: the moved pixels
        // would meet the other input's unmoved ones, so the move collapses.
        {
            Graph::GraphModel twoPins;
            const uint32_t in = twoPins.AddNode(InputNode());
            auto blendNode = ShaderLabNode(L"Gamut Map");
            blendNode.customEffect->inputNames.push_back(L"Other");
            blendNode.customEffect->lookupInputCount = 0;
            blendNode.customEffect->shiftInvariant = true;
            const uint32_t blend = twoPins.AddNode(std::move(blendNode));
            twoPins.Connect(in, 0, blend, 0);
            CleanAll(twoPins);
            DirtyRegion movedTwo;
            movedTwo.AddMove(m1);
            Graph::MarkNodeChanged(twoPins, in, movedTwo);
            twoPins.PropagateDirtyDownstream();
            TEST("Map_TwoImagePinsCollapses", twoPins.FindNode(blend)->customEffect->inputNames.size() == 2 &&
                                              !RegionOf(twoPins, blend).HasMoves() &&
                                              Holds(RegionOf(twoPins, blend), { m1.destination }));
        }

        // The flag saves and loads with the effect; absent reads false.
        const std::wstring json(graph.ToJson());
        auto loaded = Graph::GraphModel::FromJson(winrt::hstring(json));
        TEST("ShiftInvariant_RoundTrips", loaded.FindNode(invariant) && loaded.FindNode(invariant)->customEffect->shiftInvariant &&
                                          loaded.FindNode(plain) && !loaded.FindNode(plain)->customEffect->shiftInvariant &&
                                          json.find(L"\"shiftInvariant\"") != std::wstring::npos);
    }
}
