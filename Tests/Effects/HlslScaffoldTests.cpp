#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Effects/HlslScaffold.h"
#include "../../Effects/ShaderCompiler.h"
#include "../../Effects/ShaderVariants.h"
#include "../../Engine/Utf8.h"

#include <cstdio>

// The Effect Designer's starting HLSL: every scaffold must compile as
// generated, in its generic build and in each option variant, and a pixel
// shader's signature must be the one D2D binds correctly.
namespace ShaderLab::Tests
{
    namespace
    {
        Graph::CustomEffectDefinition MakeDefinition(Graph::CustomShaderType type, uint32_t inputs)
        {
            Graph::CustomEffectDefinition def;
            def.shaderType = type;
            for (uint32_t i = 0; i < inputs; ++i)
                def.inputNames.push_back(std::format(L"Source{}", i));

            Graph::ParameterDefinition gain;
            gain.name = L"Gain";
            gain.typeName = L"float";
            gain.defaultValue = 1.0f;
            def.parameters.push_back(gain);

            Graph::ParameterDefinition mode;
            mode.name = L"Mode";
            mode.typeName = L"uint";
            mode.defaultValue = 0u;
            mode.enumLabels = { L"A", L"B", L"C" };
            mode.specialize = true;
            def.parameters.push_back(mode);

            if (type != Graph::CustomShaderType::PixelShader)
            {
                def.analysisOutputType = Graph::AnalysisOutputType::Typed;
                Graph::AnalysisFieldDescriptor mean;
                mean.name = L"Mean";
                mean.type = Graph::AnalysisFieldType::Float4;
                def.analysisFields.push_back(mean);
            }
            return def;
        }

        Effects::ShaderCompileResult Compile(const Graph::CustomEffectDefinition& def,
                                             const std::vector<Effects::MacroPair>& pairs)
        {
            std::vector<Effects::ShaderCompiler::MacroDef> macros;
            for (const auto& [name, value] : pairs) macros.push_back({ name.c_str(), value.c_str() });
            return Effects::ShaderCompiler::CompileFromString(WideToUtf8(Effects::GenerateEffectScaffold(def)),
                "scaffold", "main", Effects::ShaderTargetFor(def), macros);
        }

        // The pixel shader's input semantics, in order.
        std::vector<std::string> InputSemantics(ID3DBlob* bytecode)
        {
            std::vector<std::string> names;
            winrt::com_ptr<ID3D11ShaderReflection> reflection;
            if (FAILED(D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                  IID_PPV_ARGS(reflection.put()))))
                return names;
            D3D11_SHADER_DESC desc{};
            reflection->GetDesc(&desc);
            for (UINT i = 0; i < desc.InputParameters; ++i)
            {
                D3D11_SIGNATURE_PARAMETER_DESC parameter{};
                reflection->GetInputParameterDesc(i, &parameter);
                names.push_back(std::format("{}{}", parameter.SemanticName, parameter.SemanticIndex));
            }
            return names;
        }
    }

    SL_TEST_CASE(TestHlslScaffoldsCompile, Tag::Fast)
    {
        printf("\n=== Effect Designer scaffolds ===\n");
        const struct { Graph::CustomShaderType type; uint32_t inputs; const char* label; } cases[] = {
            { Graph::CustomShaderType::PixelShader, 0, "Pixel0" },
            { Graph::CustomShaderType::PixelShader, 1, "Pixel1" },
            { Graph::CustomShaderType::PixelShader, 3, "Pixel3" },
            { Graph::CustomShaderType::ComputeShader, 1, "D2DCompute" },
            { Graph::CustomShaderType::D3D11ComputeShader, 1, "D3D11Compute" },
        };
        for (const auto& c : cases)
        {
            const auto def = MakeDefinition(c.type, c.inputs);
            const auto generic = Compile(def, Effects::GenericVariantMacros(def));
            if (!generic.succeeded)
                printf("  [info] %s: %ls\n", c.label, generic.ErrorMessage().c_str());
            TEST(std::format("Scaffold_{}_GenericCompiles", c.label).c_str(), generic.succeeded);

            // Every option variant compiles too.
            bool variantsCompile = true;
            const auto keys = Effects::AllOptionKeys(def);
            for (uint64_t key : keys)
            {
                auto macros = Effects::BuildVariantMacros(Effects::GpuBindableNames(def), 0,
                                                          Effects::SpecializedOptionNames(def), key);
                for (auto& field : Effects::AnalysisFieldMacros(def)) macros.push_back(field);
                variantsCompile = variantsCompile && Compile(def, macros).succeeded;
            }
            TEST(std::format("Scaffold_{}_VariantsCompile", c.label).c_str(), variantsCompile && keys.size() == 3);

            if (c.type == Graph::CustomShaderType::PixelShader && generic.succeeded)
            {
                // SV_POSITION, SCENE_POSITION, then TEXCOORD0..N-1, one per input.
                std::vector<std::string> expected{ "SV_POSITION0", "SCENE_POSITION0" };
                for (uint32_t i = 0; i < c.inputs; ++i) expected.push_back(std::format("TEXCOORD{}", i));
                TEST(std::format("Scaffold_{}_Signature", c.label).c_str(),
                     InputSemantics(generic.bytecode.get()) == expected);
            }
        }
    }
}
