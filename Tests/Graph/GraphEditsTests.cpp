#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Graph/GraphEdits.h"
#include "../../Graph/ValueFormat.h"
#include "../../Effects/EffectUpgrade.h"
#include "../../Effects/NodeCatalog.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceNodeFactory.h"

#include <cstdio>

// The graph edits and value formatting the Properties panel uses, tested
// without a window.
namespace ShaderLab::Tests
{
    namespace
    {
        Graph::EffectNode CreateBuiltIn(const wchar_t* name)
        {
            const auto* desc = Effects::ShaderLabEffects::Instance().FindByName(name);
            return desc ? Effects::ShaderLabEffects::CreateNode(*desc) : Graph::EffectNode{};
        }
    }

    SL_TEST_CASE(TestValueFormat, Tag::Fast)
    {
        printf("\n=== Value Format ===\n");
        Graph::AnalysisFieldValue scalar;
        scalar.name = L"Mean";
        scalar.type = Graph::AnalysisFieldType::Float3;
        scalar.components = { 1.0f, 0.5f, 0.25f, 9.0f };
        TEST("Field_Scalar", Graph::FormatAnalysisField(scalar, 0) == L"1.0000  0.5000  0.2500");

        Graph::AnalysisFieldValue array;
        array.type = Graph::AnalysisFieldType::Float2Array;
        array.arrayData = { 1, 2, 3, 4, 5, 6 };
        TEST("Field_ArrayCountOnly", Graph::FormatAnalysisField(array, 0) == L"[3 elements]");
        TEST("Field_ArrayTruncated", Graph::FormatAnalysisField(array, 2) ==
            L"[3 elements]\n  [0] 1.0000  2.0000\n  [1] 3.0000  4.0000\n  ... (1 more)\n");

        Graph::AnalysisFieldValue scalarArray;
        scalarArray.type = Graph::AnalysisFieldType::FloatArray;
        scalarArray.arrayData = { 0.5f };
        TEST("Field_ScalarArrayFull", Graph::FormatAnalysisField(scalarArray, 32) ==
            L"[1 elements]\n  [0] 0.5000\n");

        using winrt::Windows::Foundation::Numerics::float3;
        TEST("Value_Float", Graph::FormatPropertyValue(1.23456f, 3) == L"1.235");
        TEST("Value_Uint", Graph::FormatPropertyValue(7u, 3) == L"7");
        TEST("Value_Int", Graph::FormatPropertyValue(-7, 3) == L"-7");
        TEST("Value_Bool", Graph::FormatPropertyValue(true, 3) == L"true");
        TEST("Value_String", Graph::FormatPropertyValue(std::wstring(L"abc"), 3) == L"abc");
        TEST("Value_Float3", Graph::FormatPropertyValue(float3{ 1, 2, 3 }, 4) == L"1.0000, 2.0000, 3.0000");
        TEST("Value_Curve", Graph::FormatPropertyValue(std::vector<float>(5), 4) == L"[5 floats]");
        TEST("Value_Matrix", Graph::FormatPropertyValue(D2D1_MATRIX_5X4_F{}, 4) == L"(matrix)");
    }

    SL_TEST_CASE(TestGraphEdits, Tag::Fast)
    {
        printf("\n=== Graph Edits ===\n");
        Graph::GraphModel graph;

        // Property edits mark the node dirty only when applied.
        const uint32_t floatId = graph.AddNode(CreateBuiltIn(L"Float Parameter"));
        graph.FindNode(floatId)->dirty = false;
        TEST("Edit_MissingKeyRefused", !Graph::EditNodeProperty(graph, floatId, L"Nope",
            [](Graph::PropertyValue&) { return true; }));
        TEST("Edit_DeclinedLeavesClean", !Graph::EditNodeProperty(graph, floatId, L"Value",
            [](Graph::PropertyValue&) { return false; }) && !graph.FindNode(floatId)->dirty);
        TEST("Edit_Applied", Graph::EditNodeProperty(graph, floatId, L"Value",
            [](Graph::PropertyValue& v) { v = 0.75f; return true; }));
        TEST("Edit_WroteAndDirtied", std::get<float>(graph.FindNode(floatId)->properties.at(L"Value")) == 0.75f &&
            graph.FindNode(floatId)->dirty);

        // Numeric Expression inputs.
        const uint32_t exprId = graph.AddNode(CreateBuiltIn(L"Numeric Expression"));
        const auto* expr = graph.FindNode(exprId);
        TEST("Expr_StartsWithOneInput", expr && Graph::ExpressionInputCount(*expr) == 1);
        TEST("Expr_LastInputNotRemovable", !Graph::CanRemoveExpressionInput(*expr) &&
            !Graph::RemoveExpressionInput(graph, exprId, L"A"));
        TEST("Expr_AddsB", Graph::AddExpressionInput(graph, exprId) == L"B");
        expr = graph.FindNode(exprId);
        TEST("Expr_BHasProperty", expr->properties.contains(L"B") && Graph::ExpressionInputCount(*expr) == 2);
        TEST("Expr_NotOnOtherNodes", Graph::AddExpressionInput(graph, floatId).empty() &&
            !Graph::CanRemoveExpressionInput(*graph.FindNode(floatId)));

        graph.FindNode(exprId)->propertyBindings[L"A"].sources = {
            Graph::ComponentSource{ floatId, L"Value", 0, 0 } };
        TEST("Expr_ExpressionNeverRemoved", !Graph::RemoveExpressionInput(graph, exprId, L"Expression"));
        TEST("Expr_RemoveA", Graph::RemoveExpressionInput(graph, exprId, L"A"));
        expr = graph.FindNode(exprId);
        TEST("Expr_ARemovedWithBinding", !expr->properties.contains(L"A") &&
            !expr->propertyBindings.contains(L"A") && Graph::ExpressionInputCount(*expr) == 1);
        TEST("Expr_ReusesFreedLetter", Graph::AddExpressionInput(graph, exprId) == L"A");

        // Pasting: fresh state, fresh GUID, only internal edges.
        auto source = CreateBuiltIn(L"Gamut Source");
        source.runtimeError = L"old";
        source.dirty = false;
        const auto clone = Graph::CloneNodeForPaste(source);
        TEST("Clone_FreshState", clone.dirty && clone.runtimeError.empty() && clone.cachedOutput == nullptr);
        TEST("Clone_FreshGuid", clone.customEffect.has_value() &&
            !IsEqualGUID(clone.customEffect->shaderGuid, source.customEffect->shaderGuid));

        const uint32_t a = graph.AddNode(CreateBuiltIn(L"Gamut Source"));
        const uint32_t b = graph.AddNode(CreateBuiltIn(L"Gamut Highlight"));
        const uint32_t outside = graph.AddNode(CreateBuiltIn(L"Gamut Highlight"));
        const std::vector<Graph::EffectEdge> edges{ { a, 0, b, 0 }, { a, 0, outside, 0 } };
        const uint32_t a2 = graph.AddNode(Graph::CloneNodeForPaste(*graph.FindNode(a)));
        const uint32_t b2 = graph.AddNode(Graph::CloneNodeForPaste(*graph.FindNode(b)));
        const std::unordered_map<uint32_t, uint32_t> idMap{ { a, a2 }, { b, b2 } };
        TEST("Paste_ConnectsInternalEdgeOnly", Graph::ConnectClonedEdges(graph, edges, idMap) == 1 &&
            graph.GetInputEdges(b2).size() == 1 && graph.GetInputEdges(b2)[0]->sourceNodeId == a2);
    }

    // Each setter writes its field and marks the node dirty when, and only
    // when, the change affects what the node renders.
    SL_TEST_CASE(TestNodeSetters, Tag::Fast)
    {
        printf("\n=== Node Setters ===\n");
        Graph::GraphModel graph;
        const uint32_t id = graph.AddNode(CreateBuiltIn(L"Gamut Highlight"));
        auto clean = [&] { graph.FindNode(id)->dirty = false; return graph.FindNode(id); };

        auto* node = clean();
        TEST("Set_PropertyWritesAndDirties", Graph::SetNodeProperty(graph, id, L"OverlayStrength", 0.25f) &&
            std::get<float>(node->properties.at(L"OverlayStrength")) == 0.25f && node->dirty);
        TEST("Set_MissingNodeRefused", !Graph::SetNodeProperty(graph, 999, L"OverlayStrength", 0.25f) &&
            !Graph::RenameNode(graph, 999, L"x") && !Graph::RetryNode(graph, 999));

        node = clean();
        TEST("Set_MediaDirties", Graph::SetNodeMedia(graph, id, L"clip.mp4") &&
            node->mediaPath == L"clip.mp4" && node->dirty);

        node = clean();
        TEST("Set_RenameDoesNotDirty", Graph::RenameNode(graph, id, L"Renamed") &&
            node->name == L"Renamed" && !node->dirty);

        node = clean();
        Graph::SetNodePosition(*node, { 10.0f, 20.0f });
        const uint32_t moved[] = { id, 999 };
        Graph::MoveNodes(graph, moved, { 1.0f, -2.0f });
        TEST("Set_MoveOffsetsWithoutDirty", node->position.x == 11.0f && node->position.y == 18.0f && !node->dirty);

        node = clean();
        node->runtimeError = L"failed";
        TEST("Set_RetryClearsErrorAndDirties", Graph::RetryNode(graph, id) && node->runtimeError.empty() && node->dirty);

        node = clean();
        node->cachedOutput = reinterpret_cast<ID2D1Image*>(static_cast<uintptr_t>(0x10));
        TEST("Set_ResetOutputDropsImage", Graph::ResetNodeOutput(graph, id) && !node->cachedOutput && node->dirty);

        node = clean();
        node->cachedOutput = reinterpret_cast<ID2D1Image*>(static_cast<uintptr_t>(0x10));
        Graph::CustomEffectDefinition replacement = *node->customEffect;
        replacement.hlslSource = L"// replaced";
        TEST("Set_ReplaceEffect", Graph::ReplaceCustomEffect(graph, id, replacement) &&
            node->customEffect->hlslSource == L"// replaced" && !node->cachedOutput && node->dirty);

        node = clean();
        TEST("Set_ComponentNeedsBinding", !Graph::SetBindingComponent(graph, id, L"OverlayStrength", 2));
        node->propertyBindings[L"OverlayStrength"].sources.push_back(Graph::ComponentSource{ 1, L"Mean", 0, 0 });
        TEST("Set_ComponentRepoints", Graph::SetBindingComponent(graph, id, L"OverlayStrength", 2) &&
            node->propertyBindings[L"OverlayStrength"].sources[0]->sourceComponent == 2 && node->dirty);

        Graph::RecordNodeGpuTime(*node, 1.5, Graph::GpuNodeState::Fused);
        TEST("Set_GpuTimeRecorded", node->lastGpuMs == 1.5 && node->gpuState == Graph::GpuNodeState::Fused);

        const auto instance = Graph::NewInstanceOf(*node);
        TEST("Set_InstanceKeepsEffect", instance.name == node->name && instance.properties.size() == node->properties.size() &&
            std::get<float>(instance.properties.at(L"OverlayStrength")) == 0.25f &&
            instance.customEffect->hlslSource == node->customEffect->hlslSource);
        TEST("Set_InstanceNewGuidNoBindings", instance.customEffect->shaderGuid != node->customEffect->shaderGuid &&
            instance.propertyBindings.empty() && instance.dirty);

        Graph::CustomEffectDefinition def;
        def.shaderType = Graph::CustomShaderType::PixelShader;
        def.inputNames = { L"A", L"B" };
        def.parameters.push_back({ L"Gain", L"float", 2.0f, 0.0f, 4.0f, 0.1f });
        const auto pixel = Effects::CreateCustomEffectNode(def, L"Authored");
        TEST("Create_PixelShaderNode", pixel.type == Graph::NodeType::PixelShader && pixel.inputPins.size() == 2 &&
            pixel.outputPins.size() == 1 && pixel.effectClsid.has_value() &&
            std::get<float>(pixel.properties.at(L"Gain")) == 2.0f && pixel.name == L"Authored");
        def.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
        const auto compute = Effects::CreateCustomEffectNode(def, L"Data");
        TEST("Create_D3D11ComputeIsDataOnly", compute.type == Graph::NodeType::ComputeShader &&
            compute.outputPins.empty() && !compute.effectClsid.has_value());
    }

    SL_TEST_CASE(TestEffectUpgrade, Tag::Fast)
    {
        printf("\n=== Effect Upgrade ===\n");
        const auto* desc = Effects::ShaderLabEffects::Instance().FindByName(L"Gamut Highlight");
        TEST("Upgrade_HasDescriptor", desc != nullptr);
        if (!desc) return;

        Graph::GraphModel graph;
        const uint32_t id = graph.AddNode(Effects::ShaderLabEffects::CreateNode(*desc));
        auto* node = graph.FindNode(id);
        TEST("Upgrade_CurrentNotOutdated", Effects::NewerDescriptorFor(*node) == nullptr);

        // An older node: lower version, one property edited, one missing, one stale.
        TEST("Upgrade_HasProperties", node->properties.contains(L"OverlayStrength") &&
            node->properties.contains(L"OverlayR"));
        ShaderLab::Graph::SetNodeProperty(*node, L"OverlayStrength", 0.125f);
        node->customEffect->shaderLabEffectVersion = desc->effectVersion - 1;
        node->properties.erase(L"OverlayR");
        ShaderLab::Graph::SetNodeProperty(*node, L"Retired", 1.0f);
        node->dirty = false;

        TEST("Upgrade_OutdatedFound", Effects::NewerDescriptorFor(*node) == desc);
        const auto upgraded = Effects::UpgradeOutdatedNodes(graph);
        node = graph.FindNode(id);
        TEST("Upgrade_ReportsNode", upgraded.size() == 1 && upgraded[0] == id);
        TEST("Upgrade_VersionCurrent", node->customEffect->shaderLabEffectVersion == desc->effectVersion &&
            Effects::NewerDescriptorFor(*node) == nullptr);
        TEST("Upgrade_KeepsValue", std::get<float>(node->properties.at(L"OverlayStrength")) == 0.125f);
        TEST("Upgrade_RestoresMissing", node->properties.contains(L"OverlayR"));
        TEST("Upgrade_DropsRetired", !node->properties.contains(L"Retired"));
        TEST("Upgrade_Dirty", node->dirty);
        TEST("Upgrade_NothingLeft", Effects::UpgradeOutdatedNodes(graph).empty());
    }

    SL_TEST_CASE(TestVideoExtensions, Tag::Fast)
    {
        printf("\n=== Video Extensions ===\n");
        const auto extensions = Effects::SourceNodeFactory::VideoExtensions();
        TEST("VideoExt_ListsMp4AndTs", std::find(extensions.begin(), extensions.end(), L".mp4") != extensions.end() &&
            std::find(extensions.begin(), extensions.end(), L".ts") != extensions.end());
        bool allAccepted = true;
        for (const auto ext : extensions)
            allAccepted = allAccepted && Effects::SourceNodeFactory::IsVideoFile(L"clip" + std::wstring(ext));
        TEST("VideoExt_EveryListedAccepted", allAccepted);
        TEST("VideoExt_CaseInsensitive", Effects::SourceNodeFactory::IsVideoFile(L"C:\\a\\Clip.MKV"));
        TEST("VideoExt_ImageRejected", !Effects::SourceNodeFactory::IsVideoFile(L"photo.png"));
    }
}
