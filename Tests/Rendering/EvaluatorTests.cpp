#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/EffectNode.h"
#include "../../Rendering/Evaluator/ComputeDispatch.h"
#include "../../Rendering/Evaluator/ParameterNodes.h"
#include "../../Effects/ShaderIncludes.h"
#include "../../Effects/ShaderLabParamsHlsl.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Graph/GraphModel.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/MathExpression.h"
#include "../../Rendering/WorkingSpaceSync.h"
#include "Graph/GraphEdits.h"

#include <cmath>
#include <cstring>
#include <limits>

// The evaluator's device-free pieces: compute cbuffer packing, output sizing
// and dispatch counts, and parameter-node fields.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Graph;
        using namespace ShaderLab::Rendering::Evaluator;

        template <typename T>
        T ReadAt(const std::vector<BYTE>& bytes, size_t offset)
        {
            T value{};
            if (offset + sizeof(T) <= bytes.size())
                std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        ReflectedCbVar Var(const wchar_t* name, UINT offset, D3D_SHADER_VARIABLE_TYPE type)
        {
            ReflectedCbVar var;
            var.name = name;
            var.offset = offset;
            var.type = type;
            return var;
        }

        SL_TEST_CASE(TestComputeConstantPacking, Tag::Fast)
        {
            ReflectedComputeCb cb;
            cb.sizeBytes = 32;
            cb.vars = {
                Var(L"Width", 0, D3D_SVT_UINT),
                Var(L"Height", 4, D3D_SVT_UINT),
                Var(L"Gain", 8, D3D_SVT_FLOAT),
                Var(L"Mode", 12, D3D_SVT_UINT),
                Var(L"_SLIdx_Level", 16, D3D_SVT_UINT),
                Var(L"Unset", 20, D3D_SVT_FLOAT),
            };
            std::map<std::wstring, PropertyValue> props{
                { L"Gain", 2.5f },
                { L"Mode", 3.0f },        // a float-stored enum into a uint slot
                { L"Level", 123.0f },     // GPU-served: its value slot is not in this build
                { L"Width", 999.0f },     // the runner's own: never packed
            };
            Rendering::GpuBindingPlan plan;
            Rendering::GpuBindingEntry entry;
            entry.paramName = L"Level";
            entry.fieldIndex = 7;
            plan.entries.push_back(entry);

            const auto bytes = PackComputeConstants(cb, props, plan);
            TEST("ComputeConstants_SkipsRunnerWidthHeight", bytes.size() == 24);
            TEST("ComputeConstants_PacksFloat", ReadAt<float>(bytes, 0) == 2.5f);
            TEST("ComputeConstants_PacksEnumAsUint", ReadAt<uint32_t>(bytes, 4) == 3u);
            TEST("ComputeConstants_PacksGpuFieldIndex", ReadAt<uint32_t>(bytes, 8) == 7u);
            TEST("ComputeConstants_LeavesUnsetZero", ReadAt<float>(bytes, 12) == 0.0f);

            ReflectedComputeCb runnerOnly;
            runnerOnly.sizeBytes = 8;
            TEST("ComputeConstants_EmptyWithoutUserConstants", PackComputeConstants(runnerOnly, props, plan).empty());
        }

        SL_TEST_CASE(TestComputeOutputSizing, Tag::Fast)
        {
            CustomEffectDefinition def;
            def.outputExtent.sizeParam = L"DiagramSize";

            auto size = ComputeOutputSizeFromProperties(def, { { L"OutputWidth", 300.0f }, { L"OutputHeight", 200u },
                                                               { L"DiagramSize", 512.0f } });
            TEST("ComputeOutputSize_ExplicitWidthHeightFirst", size.width == 300 && size.height == 200);

            size = ComputeOutputSizeFromProperties(def, { { L"OutputWidth", 300.0f }, { L"DiagramSize", 10.0f } });
            TEST("ComputeOutputSize_SquareSizeAtLeast64", size.width == 64 && size.height == 64);

            size = ComputeOutputSizeFromProperties(def, { { L"DiagramSize", 256u } });
            TEST("ComputeOutputSize_SquareSizeUint", size.width == 256 && size.height == 256);

            size = ComputeOutputSizeFromProperties(CustomEffectDefinition{}, { { L"OutputWidth", 0.5f }, { L"OutputHeight", 9.0f } });
            TEST("ComputeOutputSize_NoneStated", size.width == 0 && size.height == 0);
        }

        SL_TEST_CASE(TestPerPixelDispatchGroups, Tag::Fast)
        {
            CustomEffectDefinition def;
            def.dispatchShape = DispatchShape::PerOutputPixel;
            def.threadGroupX = 16;
            def.threadGroupY = 4;
            ReflectedComputeCb reflected;
            reflected.groupX = 8;
            reflected.groupY = 8;

            auto groups = PerPixelDispatchGroups(def, reflected, 100, 50);
            TEST("DispatchGroups_FromNumThreads", groups && (*groups)[0] == 13 && (*groups)[1] == 7 && (*groups)[2] == 1);

            groups = PerPixelDispatchGroups(def, ReflectedComputeCb{}, 100, 50);
            TEST("DispatchGroups_DefinitionWithoutReflection", groups && (*groups)[0] == 7 && (*groups)[1] == 13);

            TEST("DispatchGroups_NoneForEmptyOutput", !PerPixelDispatchGroups(def, reflected, 0, 50));
            def.dispatchShape = DispatchShape::SingleGroup;
            TEST("DispatchGroups_NoneForSingleGroup", !PerPixelDispatchGroups(def, reflected, 100, 50));
        }

        EffectNode ParameterNode(NodeKind kind, std::vector<AnalysisFieldDescriptor> fields)
        {
            EffectNode node;
            node.type = NodeType::ComputeShader;
            CustomEffectDefinition def;
            def.nodeKind = kind;
            def.analysisFields = std::move(fields);
            node.customEffect = std::move(def);
            return node;
        }

        float FieldValue(const std::vector<AnalysisFieldValue>& fields, const wchar_t* name, uint32_t component = 0)
        {
            for (const auto& field : fields)
                if (field.name == name) return field.components[component];
            return std::numeric_limits<float>::quiet_NaN();
        }

        SL_TEST_CASE(TestParameterNodeFields, Tag::Fast)
        {
            auto clock = ParameterNode(NodeKind::Clock, {
                { L"Time", AnalysisFieldType::Float }, { L"Progress", AnalysisFieldType::Float } });
            clock.properties = { { L"StartTime", 2.0f }, { L"StopTime", 12.0f }, { L"UpdateRate", 4.0f } };
            clock.clockTime = 1.3;
            const auto clockFields = ParameterNodeFields(clock);
            TEST("ParameterNode_ClockQuantisesToRate", FieldValue(clockFields, L"Time") == 3.25f);
            TEST("ParameterNode_ClockProgress", FieldValue(clockFields, L"Progress") == 0.125f);

            auto random = ParameterNode(NodeKind::Random, { { L"Result", AnalysisFieldType::Float } });
            ShaderLab::Graph::SetNodeProperty(random, L"Seed", 1.0f);
            const float first = FieldValue(ParameterNodeFields(random), L"Result");
            const float again = FieldValue(ParameterNodeFields(random), L"Result");
            ShaderLab::Graph::SetNodeProperty(random, L"Seed", 2.0f);
            const float other = FieldValue(ParameterNodeFields(random), L"Result");
            TEST("ParameterNode_RandomRepeatable", first == again);
            TEST("ParameterNode_RandomInUnitRange", first >= 0.0f && first < 1.0f && other >= 0.0f && other < 1.0f);
            TEST("ParameterNode_RandomFollowsSeed", first != other);


            namespace Numerics = winrt::Windows::Foundation::Numerics;
            auto constant = ParameterNode(NodeKind::ConstantParameter, {
                { L"Primary", AnalysisFieldType::Float3 }, { L"Enabled", AnalysisFieldType::Float } });
            constant.properties = { { L"Primary", Numerics::float3{ 0.25f, 0.5f, 0.75f } }, { L"Enabled", true } };
            const auto constantFields = ParameterNodeFields(constant);
            TEST("ParameterNode_ConstantCopiesVector",
                 FieldValue(constantFields, L"Primary", 0) == 0.25f && FieldValue(constantFields, L"Primary", 2) == 0.75f);
            TEST("ParameterNode_ConstantCopiesBool", FieldValue(constantFields, L"Enabled") == 1.0f);
        }

        SL_TEST_CASE(TestWorkingSpaceSync, Tag::Fast)
        {
            const auto* desc = ShaderLab::Effects::ShaderLabEffects::Instance().FindById(L"Working Space");
            TEST("WorkingSpace_DescriptorExists", desc != nullptr);
            if (!desc) return;
            GraphModel graph;
            const uint32_t id = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc));
            const auto readFloat = [&](const wchar_t* name) {
                const auto* node = graph.FindNode(id);
                const auto it = node->properties.find(name);
                const float* value = (it != node->properties.end()) ? std::get_if<float>(&it->second) : nullptr;
                return value ? *value : std::numeric_limits<float>::quiet_NaN();
            };

            auto profile = ShaderLab::Rendering::PresetBT2020_1000();
            graph.FindNode(id)->dirty = false;
            TEST("WorkingSpace_ChangeReported", ShaderLab::Rendering::UpdateWorkingSpaceNodes(graph, profile));
            TEST("WorkingSpace_PeakCopied", readFloat(L"PeakNits") == profile.caps.maxLuminanceNits);
            TEST("WorkingSpace_SimulatedFromProfile", readFloat(L"IsSimulated") == 1.0f);
            TEST("WorkingSpace_ChangedNodeDirty", graph.FindNode(id)->dirty);

            graph.FindNode(id)->dirty = false;
            profile.caps.maxLuminanceNits += 0.5f;
            TEST("WorkingSpace_DeadBandHoldsStable", !ShaderLab::Rendering::UpdateWorkingSpaceNodes(graph, profile)
                 && !graph.FindNode(id)->dirty);
        }
    }

    SL_TEST_CASE(TestShaderIncludes, Tag::Fast)
    {
        std::printf("\n=== Embedded shader includes ===\n");
        using namespace ShaderLab::Effects;
        const auto includes = EmbeddedIncludes();
        TEST("ShaderIncludes_ThreeInOrder", includes.size() == 3 &&
             includes[0].name == "shaderlab_params.hlsli" && includes[1].name == "shaderlab_colormath.hlsli" &&
             includes[2].name == "shaderlab_gamut.hlsli");
        const auto params = FindEmbeddedInclude("shaderlab_params.hlsli");
        TEST("ShaderIncludes_FindsByName", params && params->data() == GetShaderLabParamsHLSL() &&
             params->size() == GetShaderLabParamsHLSLLength());
        TEST("ShaderIncludes_NameIsExact", !FindEmbeddedInclude("Shaderlab_params.hlsli") &&
             !FindEmbeddedInclude("include/shaderlab_params.hlsli"));

        const void* data = nullptr;
        UINT bytes = 0;
        ID3DInclude* handler = ShaderLabIncludeHandler();
        TEST("ShaderIncludes_HandlerServesText", SUCCEEDED(handler->Open(D3D_INCLUDE_LOCAL, "shaderlab_gamut.hlsli",
             nullptr, &data, &bytes)) && data == includes[2].text.data() && bytes == includes[2].text.size());
        TEST("ShaderIncludes_HandlerRefusesOthers",
             FAILED(handler->Open(D3D_INCLUDE_LOCAL, "missing.hlsli", nullptr, &data, &bytes)));
    }

    // The engine has no expression language of its own: a host registers one,
    // and without it every expression fails with a reason.
    SL_TEST_CASE(TestMathExpressionHook, Tag::Fast)
    {
        printf("\n=== Numeric Expression evaluator hook ===\n");
        const wchar_t* names[] = { L"A", L"B" };
        const float values[] = { 3.0f, 4.0f };
        float result = 1.0f;
        std::wstring error;
        TEST("MathHook_UnsetFails", !Rendering::EvaluateMathExpression(L"A + B", names, values, 2, result, &error) &&
             result == 0.0f && !error.empty());

        Rendering::SetMathExpressionEvaluator([](const std::wstring& expression, const wchar_t* const*, const float* inputs,
                                                 size_t count, float& out, std::wstring*) {
            out = expression == L"A + B" && count == 2 ? inputs[0] + inputs[1] : 0.0f;
            return true;
        });
        const bool evaluated = Rendering::EvaluateMathExpression(L"A + B", names, values, 2, result, &error);
        Rendering::SetMathExpressionEvaluator(nullptr);
        TEST("MathHook_RegisteredIsUsed", evaluated && result == 7.0f);
    }
}
