#include "pch_engine.h"
#include "NodeCatalog.h"
#include "../Graph/GraphModel.h"
#include "BuiltInEffects.h"
#include "CustomComputeShaderEffect.h"
#include "CustomPixelShaderEffect.h"
#include "EffectRegistry.h"
#include "ShaderLabEffects.h"
#include "SourceNodeFactory.h"

#include <shared_mutex>

namespace ShaderLab::Effects
{
    namespace
    {
        Graph::EffectNode CreateCustomShaderNode(std::wstring_view name)
        {
            const bool isCompute = name == L"Custom Compute Shader";
            const bool isD3D11   = name == L"Custom D3D11 Compute Shader";
            Graph::EffectNode node;
            node.type = (isCompute || isD3D11) ? Graph::NodeType::ComputeShader : Graph::NodeType::PixelShader;
            node.name = std::wstring(name);
            if (!isD3D11)
            {
                node.effectClsid = isCompute
                    ? CustomComputeShaderEffect::CLSID_CustomComputeShader
                    : CustomPixelShaderEffect::CLSID_CustomPixelShader;
                node.outputPins.push_back({ L"Output", 0 });
            }

            Graph::CustomEffectDefinition def;
            def.shaderType = isD3D11 ? Graph::CustomShaderType::D3D11ComputeShader
                           : isCompute ? Graph::CustomShaderType::ComputeShader
                           : Graph::CustomShaderType::PixelShader;
            CoCreateGuid(&def.shaderGuid);
            def.inputNames.push_back(L"Source");
            node.inputPins.push_back({ L"I0", 0 });
            if (isCompute) { def.threadGroupX = 8; def.threadGroupY = 8; def.threadGroupZ = 1; }
            if (isD3D11)
            {
                def.analysisOutputType = Graph::AnalysisOutputType::Typed;
                def.analysisFields.push_back({ L"Result", Graph::AnalysisFieldType::Float4 });
            }
            Graph::DeriveContracts(def, node.properties);
            node.customEffect = std::move(def);
            return node;
        }

        struct NodeMakers
        {
            std::shared_mutex mutex;
            std::unordered_map<std::wstring, std::shared_ptr<const NodeMaker>> byName;
        };

        NodeMakers& Makers()
        {
            static NodeMakers makers;
            return makers;
        }

        std::shared_ptr<const NodeMaker> FindNodeMaker(std::wstring_view name)
        {
            auto& makers = Makers();
            std::shared_lock lock(makers.mutex);
            auto it = makers.byName.find(std::wstring(name));
            return it == makers.byName.end() ? nullptr : it->second;
        }

        // The file name, or `fallback` without a file.
        std::wstring DisplayName(const std::wstring& filePath, const wchar_t* fallback)
        {
            return filePath.empty() ? std::wstring(fallback) : filePath.substr(filePath.find_last_of(L"\\/") + 1);
        }
    }

    void RegisterNodeMaker(std::vector<std::wstring> names, NodeMaker maker)
    {
        if (!maker) return;
        auto shared = std::make_shared<const NodeMaker>(std::move(maker));
        auto& makers = Makers();
        std::unique_lock lock(makers.mutex);
        for (auto& name : names)
            makers.byName[std::move(name)] = shared;
    }

    Graph::EffectNode CreateCustomEffectNode(Graph::CustomEffectDefinition definition, std::wstring name)
    {
        using Graph::CustomShaderType;
        Graph::EffectNode node;
        node.type = definition.shaderType == CustomShaderType::PixelShader
            ? Graph::NodeType::PixelShader : Graph::NodeType::ComputeShader;
        node.name = std::move(name);
        for (uint32_t i = 0; i < definition.inputNames.size(); ++i)
            node.inputPins.push_back({ definition.inputNames[i], i });
        if (definition.shaderType != CustomShaderType::D3D11ComputeShader)
        {
            node.outputPins.push_back({ L"Output", 0 });
            node.effectClsid = definition.shaderType == CustomShaderType::PixelShader
                ? CustomPixelShaderEffect::CLSID_CustomPixelShader
                : CustomComputeShaderEffect::CLSID_CustomComputeShader;
        }
        for (const auto& parameter : definition.parameters)
            node.properties[parameter.name] = parameter.defaultValue;
        Graph::DeriveContracts(definition, node.properties);
        node.customEffect = std::move(definition);
        return node;
    }

    NodeCreateResult CreateNodeByName(
        std::wstring_view name, const NodeCreateArgs& args, const Graph::GraphModel& graph)
    {
        NodeCreateResult result;
        if (name == L"Custom Compute Shader" || name == L"Custom Pixel Shader" ||
            name == L"Custom D3D11 Compute Shader")
        {
            result.node = CreateCustomShaderNode(name);
            return result;
        }
        if (auto* descriptor = ShaderLabEffects::Instance().FindByName(name))
        {
            result.node = ShaderLabEffects::CreateNode(*descriptor);
            return result;
        }
        if (auto* descriptor = EffectRegistry::Instance().FindByName(name))
        {
            result.node = EffectRegistry::CreateNode(*descriptor);
            return result;
        }

        // A node type, not an effect. In the GUI each Output gets a window.
        if (name == L"Output")
        {
            result.node = EffectRegistry::CreateOutputNode();
            const size_t outputCount = graph.GetOutputNodeIds().size();
            if (outputCount > 0)
                result.node->name = L"Output " + std::to_wstring(outputCount + 1);
            return result;
        }

        if (name == L"Video Source" || name == L"Video")
        {
            result.node = SourceNodeFactory::CreateVideoSourceNode(args.filePath, DisplayName(args.filePath, L"Video Source"));
            result.needsPrepare = !args.filePath.empty();
            return result;
        }
        if (name == L"Image Source" || name == L"Image")
        {
            result.node = SourceNodeFactory::CreateImageSourceNode(args.filePath, DisplayName(args.filePath, L"Image Source"));
            result.needsPrepare = !args.filePath.empty();
            return result;
        }

        if (auto maker = FindNodeMaker(name))
            return (*maker)(args, graph);

        result.error = MissingBuiltInMessage(name);
        if (result.error.empty()) result.error = L"Unknown effect name: " + std::wstring(name);
        return result;
    }
}
