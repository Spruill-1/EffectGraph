#include "pch_engine.h"
#include "GpuBindingPlan.h"

namespace ShaderLab::Rendering
{
    using Effects::GpuBindingMode;

    GpuBindingMode GpuLaneFor(const Graph::EffectNode& consumer)
    {
        if (!consumer.customEffect.has_value()) return GpuBindingMode::CBuffer;
        switch (consumer.customEffect->shaderType)
        {
        case Graph::CustomShaderType::D3D11ComputeShader: return GpuBindingMode::StructuredBuffer;
        case Graph::CustomShaderType::PixelShader:
            return consumer.type == Graph::NodeType::PixelShader ? GpuBindingMode::Texture : GpuBindingMode::CBuffer;
        default:                                          return GpuBindingMode::CBuffer;
        }
    }

    std::optional<uint32_t> GpuBindingFieldIndex(
        const Graph::PropertyBinding& binding, const Graph::EffectNode& producer)
    {
        using namespace Graph;
        if (binding.wholeArray || binding.sources.empty() || !producer.customEffect.has_value())
            return std::nullopt;
        const auto& first = binding.sources[0];
        if (!first.has_value())
            return std::nullopt;
        for (size_t i = 0; i < binding.sources.size(); ++i)
        {
            const auto& source = binding.sources[i];
            if (!source.has_value() ||
                source->sourceNodeId != first->sourceNodeId ||
                source->sourceFieldName != first->sourceFieldName ||
                source->sourceIndex != first->sourceIndex ||
                source->sourceComponent != i)
                return std::nullopt;
        }

        uint32_t fieldIndex = 0;
        for (const auto& fieldDesc : producer.customEffect->analysisFields)
        {
            if (fieldDesc.name != first->sourceFieldName)
            {
                fieldIndex += fieldDesc.pixelCount();
                continue;
            }
            if (binding.sources.size() > AnalysisFieldComponentCount(fieldDesc.type))
                return std::nullopt;
            if (!AnalysisFieldIsArray(fieldDesc.type))
            {
                if (first->sourceIndex != 0) return std::nullopt;
                return fieldIndex;
            }
            if (first->sourceIndex >= fieldDesc.arrayLength)
                return std::nullopt;
            if (fieldDesc.type == AnalysisFieldType::FloatArray)
            {
                // Four elements per pixel: only an element in .x is servable.
                if (first->sourceIndex % 4 != 0) return std::nullopt;
                return fieldIndex + first->sourceIndex / 4;
            }
            return fieldIndex + first->sourceIndex;
        }
        return std::nullopt;
    }

    GpuBindingPlan PlanGpuBindings(
        const Graph::EffectNode& consumer,
        const Graph::GraphModel& graph,
        bool gpuBindingsEnabled,
        const std::function<bool(uint32_t producerId)>& producerReady)
    {
        GpuBindingPlan plan;
        const GpuBindingMode lane = GpuLaneFor(consumer);
        if (!gpuBindingsEnabled || lane == GpuBindingMode::CBuffer)
            return plan;

        uint32_t gpuBindableIndex = 0;
        for (const auto& param : consumer.customEffect->parameters)
        {
            if (!param.gpuBindable) continue;
            const uint32_t index = gpuBindableIndex++;
            if (index >= Effects::cMaxGpuBindableParams) break;

            auto bindingIt = consumer.propertyBindings.find(param.name);
            if (bindingIt == consumer.propertyBindings.end()) continue;
            const auto& binding = bindingIt->second;
            if (binding.wholeArray || binding.sources.empty() || !binding.sources[0].has_value()) continue;

            // Only a D3D11 compute node publishes its analysis row on the GPU.
            const uint32_t producerId = binding.sources[0]->sourceNodeId;
            const auto* producer = graph.FindNode(producerId);
            if (!producer || !producer->customEffect.has_value() ||
                producer->customEffect->shaderType != Graph::CustomShaderType::D3D11ComputeShader)
                continue;
            const auto fieldIndex = GpuBindingFieldIndex(binding, *producer);
            if (!fieldIndex) continue;
            if (producerReady && !producerReady(producerId)) continue;

            plan.entries.push_back({ param.name, index, lane, producerId, *fieldIndex });
            plan.bits = Effects::WithBindingMode(plan.bits, index, lane);
        }
        return plan;
    }
}
