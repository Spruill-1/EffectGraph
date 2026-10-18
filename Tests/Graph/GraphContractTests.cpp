#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "Engine/Api/EffectGraphVersion.h"
#include "../../Graph/NodeContracts.h"
#include "../../Graph/SourceKind.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/ShaderVariants.h"
#include "../../Effects/ShaderCompiler.h"
#include "../../Effects/SourceNodeFactory.h"
#include "Graph/GraphEdits.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

// The contracts a node declares to the evaluator (NodeContracts.h), the
// source classifier, and loading: old graphs derive what they lack, bad
// references are dropped and reported, and every committed fixture loads.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Graph;
        namespace WDJ = winrt::Windows::Data::Json;

        std::filesystem::path FixtureDir()
        {
            wchar_t buf[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::filesystem::path dir = std::filesystem::path(buf).parent_path();
            for (int i = 0; i < 6 && !dir.empty(); ++i)
            {
                auto candidate = dir / L"Tests" / L"fixtures";
                if (std::filesystem::exists(candidate)) return candidate;
                dir = dir.parent_path();
            }
            return {};
        }

        std::wstring ReadText(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
            return winrt::to_hstring(bytes).c_str();
        }

        bool SameContracts(const CustomEffectDefinition& a, const CustomEffectDefinition& b)
        {
            return a.nodeKind == b.nodeKind && a.dispatchShape == b.dispatchShape &&
                   a.outputExtent == b.outputExtent && a.injectedConstants == b.injectedConstants;
        }

        // A graph's JSON with the contract keys removed: what a build before
        // them saved.
        std::wstring StripContracts(const winrt::hstring& json)
        {
            auto root = WDJ::JsonObject::Parse(json);
            auto nodes = root.GetNamedArray(L"nodes");
            for (uint32_t i = 0; i < nodes.Size(); ++i)
            {
                auto node = nodes.GetObjectAt(i);
                if (!node.HasKey(L"customEffect")) continue;
                auto ced = node.GetNamedObject(L"customEffect");
                for (const wchar_t* key : { L"nodeKind", L"dispatchShape", L"outputExtent", L"injectedConstants" })
                    ced.Remove(key);
            }
            return std::wstring(root.Stringify());
        }

        SL_TEST_CASE(TestDerivation, Tag::Fast)
        {
            std::printf("\n=== Node contracts: derivation ===\n");
            CustomEffectDefinition compute;
            compute.shaderType = CustomShaderType::D3D11ComputeShader;
            compute.threadGroupX = compute.threadGroupY = 8;
            TEST("Derive_SmallGroupTiles", DeriveDispatchShape(compute) == DispatchShape::PerOutputPixel);
            compute.threadGroupX = compute.threadGroupY = 16;
            TEST("Derive_256ThreadsIsOneGroup", DeriveDispatchShape(compute) == DispatchShape::SingleGroup);
            compute.threadGroupX = 8; compute.threadGroupY = 1;
            TEST("Derive_OneRowGroupIsOneGroup", DeriveDispatchShape(compute) == DispatchShape::SingleGroup);
            compute.threadGroupY = 8;
            compute.parameters.push_back({ L"OutputSize", L"uint", 512u });
            TEST("Derive_OutputSizeParamIsOneGroup", DeriveDispatchShape(compute) == DispatchShape::SingleGroup);
            TEST("Derive_ComputeExtentFromParam", DeriveOutputExtent(compute, {}) == OutputExtent{ L"OutputSize" });

            CustomEffectDefinition pixel;
            pixel.hlslSource = L"x";
            std::map<std::wstring, PropertyValue> props{ { L"GradSize", 300.0f }, { L"ASize", 7u } };
            TEST("Derive_GeneratorTakesFirstFloatSize", DeriveOutputExtent(pixel, props) == OutputExtent{ L"GradSize" });
            props[L"PatchSize"] = 40.0f;
            TEST("Derive_PatchSizeIsSixByFour", DeriveOutputExtent(pixel, props) == (OutputExtent{ L"PatchSize", 6, 4 }));
            props[L"DiagramSize"] = 512.0f;
            TEST("Derive_DiagramSizeFirst", DeriveOutputExtent(pixel, props) == OutputExtent{ L"DiagramSize" });
            pixel.inputNames = { L"Source" };
            TEST("Derive_FilterWithoutSizeHasNoExtent",
                 DeriveOutputExtent(pixel, { { L"GradSize", 300.0f } }).sizeParam.empty());

            TEST("Derive_InjectedFromNames",
                 DeriveInjectedConstants({ { L"InputMask", 0.0f }, { L"OutputH", 1.0f } }) ==
                     (InjectedConstants::InputMask | InjectedConstants::OutputSize) &&
                 DeriveInjectedConstants({ { L"Gain", 1.0f } }) == InjectedConstants::None);

            CustomEffectDefinition parameter;
            parameter.analysisOutputType = AnalysisOutputType::Typed;
            parameter.analysisFields = { { L"Value", AnalysisFieldType::Float } };
            TEST("Derive_NoHlslTypedIsParameter", DeriveNodeKind(parameter) == NodeKind::ConstantParameter);
            parameter.shaderLabEffectId = cRandomEffectId;
            TEST("Derive_KindFromEffectId", DeriveNodeKind(parameter) == NodeKind::Random);
            parameter.hlslSource = L"x";
            TEST("Derive_HlslIsShader", DeriveNodeKind(parameter) == NodeKind::Shader);

            CustomEffectDefinition blankD3D11;
            blankD3D11.shaderType = CustomShaderType::D3D11ComputeShader;
            blankD3D11.analysisOutputType = AnalysisOutputType::Typed;
            blankD3D11.analysisFields = { { L"Result", AnalysisFieldType::Float4 } };
            TEST("Derive_BlankD3D11IsShader", DeriveNodeKind(blankD3D11) == NodeKind::Shader);

            TEST("Tags_RoundTrip",
                 NodeKindFromTag(NodeKindTag(NodeKind::WorkingSpace)) == NodeKind::WorkingSpace &&
                 DispatchShapeFromTag(DispatchShapeTag(DispatchShape::ImagePass)) == DispatchShape::ImagePass &&
                 !NodeKindFromTag(L"bogus").has_value());
        }

        // Every built-in declares what an old graph of it derives, so a
        // graph saved before the contracts renders exactly as one saved after.
        SL_TEST_CASE(TestBuiltInsMatchDerivation, Tag::Fast)
        {
            std::printf("\n=== Node contracts: built-ins match derivation ===\n");
            int mismatches = 0;
            for (const auto& desc : Effects::ShaderLabEffects::Instance().All())
            {
                if (desc.IsUserEffect()) continue;
                GraphModel graph;
                graph.AddNode(Effects::ShaderLabEffects::CreateNode(desc));
                const auto legacy = GraphModel::FromJson(winrt::hstring(StripContracts(graph.ToJson())));
                const auto& declared = *graph.Nodes()[0].customEffect;
                const auto& derived = *legacy.Nodes()[0].customEffect;

                bool same = declared.nodeKind == derived.nodeKind &&
                            declared.outputExtent == derived.outputExtent &&
                            declared.injectedConstants == derived.injectedConstants;
                // The dispatch shape matters only where the evaluator sizes it.
                if (desc.shaderType == CustomShaderType::D3D11ComputeShader && desc.hasImageOutput)
                    same = same && TilesPerOutputPixel(declared.dispatchShape) == TilesPerOutputPixel(derived.dispatchShape);
                if (!same)
                {
                    ++mismatches;
                    std::printf("    %ls: declared and derived contracts differ\n", desc.name.c_str());
                }
            }
            TEST("Contracts_BuiltInsMatchDerivation", mismatches == 0);
        }

        // The Effect Designer's "Update in graph" hands over a definition
        // with no contracts and no hidden values; deriving against the node's
        // own properties keeps what the host injects.
        SL_TEST_CASE(TestDesignerUpdateKeepsInjected, Tag::Fast)
        {
            std::printf("\n=== Node contracts: designer update ===\n");
            const auto* desc = Effects::ShaderLabEffects::Instance().FindByName(L"Split Comparison");
            if (!desc)
            {
                TEST("Designer_SplitComparisonPresent", false);
                return;
            }
            const auto node = Effects::ShaderLabEffects::CreateNode(*desc);

            CustomEffectDefinition authored;
            authored.shaderType = node.customEffect->shaderType;
            authored.hlslSource = node.customEffect->hlslSource;
            authored.inputNames = node.customEffect->inputNames;
            authored.parameters = node.customEffect->parameters;

            std::map<std::wstring, PropertyValue> defaults;
            for (const auto& p : authored.parameters) defaults[p.name] = p.defaultValue;
            TEST("Designer_ParameterDefaultsLackHiddenValues",
                 DeriveInjectedConstants(defaults) == InjectedConstants::None);

            DeriveContracts(authored, node.properties);
            TEST("Designer_UpdateKeepsInjected", authored.injectedConstants ==
                 (InjectedConstants::InputMask | InjectedConstants::OutputSize));
            // A pixel shader's dispatch shape is not read, so it is not compared.
            TEST("Designer_UpdateMatchesDeclared", authored.nodeKind == node.customEffect->nodeKind &&
                 authored.outputExtent == node.customEffect->outputExtent);
        }

        // A runner-sized shape is what the HLSL itself asks the runner for.
        SL_TEST_CASE(TestDispatchShapesMatchHlsl, Tag::Fast)
        {
            std::printf("\n=== Node contracts: dispatch shapes match HLSL ===\n");
            int mismatches = 0;
            for (const auto& desc : Effects::ShaderLabEffects::Instance().All())
            {
                if (desc.IsUserEffect() || desc.shaderType != CustomShaderType::D3D11ComputeShader) continue;
                const bool reduces = desc.hlslSource.find("SHADERLAB_REDUCE_SCRATCH") != std::string::npos;
                const bool imagePass = desc.hlslSource.find("SHADERLAB_IMAGE_PASS(") != std::string::npos;
                const bool ok = reduces == (desc.dispatchShape == DispatchShape::ReduceGroups) &&
                                imagePass == (desc.dispatchShape == DispatchShape::ImagePass);
                if (!ok)
                {
                    ++mismatches;
                    std::printf("    %ls: dispatchShape %ls disagrees with its HLSL\n",
                        desc.name.c_str(), DispatchShapeTag(desc.dispatchShape));
                }
            }
            TEST("Contracts_DispatchShapesMatchHlsl", mismatches == 0);
        }

        // Result[...] in a built-in uses SL_FIELD_<name> (injected from its
        // analysisFields), never a literal index, and each name exists.
        SL_TEST_CASE(TestAnalysisFieldIndices, Tag::Fast)
        {
            std::printf("\n=== Node contracts: analysis field indices ===\n");
            const std::regex literalIndex(R"(Result\[\s*\d+\s*\])");
            const std::regex fieldMacro(R"(SL_FIELD_(\w+))");
            int problems = 0;
            for (const auto& desc : Effects::ShaderLabEffects::Instance().All())
            {
                if (desc.IsUserEffect() || desc.analysisFields.empty()) continue;
                if (std::regex_search(desc.hlslSource, literalIndex))
                {
                    ++problems;
                    std::printf("    %ls writes Result[<literal>]\n", desc.name.c_str());
                }
                CustomEffectDefinition def;
                def.analysisFields = desc.analysisFields;
                std::set<std::string> defined;
                for (const auto& [name, value] : Effects::AnalysisFieldMacros(def))
                    defined.insert(name);
                for (std::sregex_iterator it(desc.hlslSource.begin(), desc.hlslSource.end(), fieldMacro), end; it != end; ++it)
                {
                    if (defined.contains(it->str(0))) continue;
                    ++problems;
                    std::printf("    %ls uses %s, which is not one of its fields\n", desc.name.c_str(), it->str(0).c_str());
                }
            }
            TEST("Contracts_ResultIndicesAreFieldMacros", problems == 0);

            CustomEffectDefinition def;
            def.analysisFields = { { L"Min", AnalysisFieldType::Float },
                                   { L"Hist", AnalysisFieldType::FloatArray, 10 },
                                   { L"Nonzero%", AnalysisFieldType::Float } };
            const auto macros = Effects::AnalysisFieldMacros(def);
            TEST("FieldMacros_ArePixelOffsets", macros.size() == 3 &&
                 macros[0] == Effects::MacroPair{ "SL_FIELD_Min", "0" } &&
                 macros[1] == Effects::MacroPair{ "SL_FIELD_Hist", "1" } &&
                 macros[2] == Effects::MacroPair{ "SL_FIELD_Nonzero_", "4" });
        }

        // A built-in's host-computed tables are exactly the size of the
        // cbuffer variables its HLSL declares for them, under every option.
        SL_TEST_CASE(TestDerivedConstantLayouts, Tag::Fast)
        {
            std::printf("\n=== Node contracts: derived constant layouts ===\n");
            const auto& hooks = Effects::BuiltInEffectHooks();
            int checked = 0, problems = 0;
            for (const auto& desc : Effects::ShaderLabEffects::Instance().All())
            {
                const auto* derive = hooks.DerivedConstants(desc.effectId);
                if (desc.IsUserEffect() || !derive) continue;
                const auto node = Effects::ShaderLabEffects::CreateNode(desc);
                const auto macroPairs = Effects::GenericVariantMacros(*node.customEffect);
                std::vector<Effects::ShaderCompiler::MacroDef> macros;
                for (const auto& [name, value] : macroPairs) macros.push_back({ name.c_str(), value.c_str() });
                auto compiled = Effects::ShaderCompiler::CompileFromString(desc.hlslSource, "derived_layout",
                    "main", Effects::ShaderTargetFor(*node.customEffect), macros);
                if (!compiled.succeeded) { ++problems; std::printf("    %ls does not compile\n", desc.name.c_str()); continue; }
                const auto reflection = Effects::ShaderCompiler::Reflect(compiled.bytecode.get());

                // The defaults, then each value of each enum parameter.
                std::vector<std::map<std::wstring, PropertyValue>> settings{ node.properties };
                for (const auto& param : desc.parameters)
                    for (size_t value = 0; value < param.enumLabels.size(); ++value)
                    {
                        auto props = node.properties;
                        props[param.name] = static_cast<float>(value);
                        settings.push_back(std::move(props));
                    }
                for (const auto& props : settings)
                {
                    (*derive)(props, [&](std::wstring_view name, const float*, size_t floatCount)
                    {
                        ++checked;
                        bool found = false;
                        for (const auto& cb : reflection.constantBuffers)
                            for (const auto& var : cb.variables)
                                if (var.name == name)
                                {
                                    found = true;
                                    if (var.size != floatCount * sizeof(float))
                                    {
                                        ++problems;
                                        std::printf("    %ls: %ls is %zu floats, the HLSL declares %u bytes\n",
                                            desc.name.c_str(), std::wstring(name).c_str(), floatCount, var.size);
                                    }
                                }
                        if (!found)
                        {
                            ++problems;
                            std::printf("    %ls: the HLSL has no %ls\n", desc.name.c_str(), std::wstring(name).c_str());
                        }
                    });
                }
            }
            TEST("Contracts_DerivedTablesFitTheirCbufferVars", checked >= 3 && problems == 0);
        }

        SL_TEST_CASE(TestLegacyFixture, Tag::Fast)
        {
            std::printf("\n=== Graph load: legacy fixture ===\n");
            const auto dir = FixtureDir();
            const auto path = dir / L"legacy_contracts.json";
            if (dir.empty() || !std::filesystem::exists(path))
            {
                TEST("Legacy_FixturePresent", false);
                return;
            }
            GraphModel graph = GraphModel::FromJson(winrt::hstring(ReadText(path)));
            auto kindOf = [&](uint32_t id) { return graph.FindNode(id)->customEffect->nodeKind; };
            TEST("Legacy_ClockIsClockWithoutPostLoadCall", kindOf(1) == NodeKind::Clock && graph.FindNode(1)->IsClock());
            TEST("Legacy_ExpressionKind", kindOf(2) == NodeKind::Expression);
            TEST("Legacy_WorkingSpaceKind", kindOf(3) == NodeKind::WorkingSpace);
            TEST("Legacy_ShaderKind", kindOf(6) == NodeKind::Shader);

            const auto* picture = graph.FindNode(4);
            TEST("Legacy_ImagePathOneStore", picture->mediaPath == L"C:\\media\\picture.png" &&
                 !picture->properties.contains(L"shaderPath") && ClassifySource(*picture) == SourceKind::Image);
            const auto* clip = graph.FindNode(5);
            TEST("Legacy_VideoPathFromPropertyCopy", clip->mediaPath == L"C:\\media\\newer.mp4" &&
                 ClassifySource(*clip) == SourceKind::Video);

            TEST("Legacy_GeneratorExtent", graph.FindNode(6)->customEffect->outputExtent == OutputExtent{ L"PlateSize" });
            TEST("Legacy_PatchExtent", graph.FindNode(7)->customEffect->outputExtent == (OutputExtent{ L"PatchSize", 6, 4 }));
            TEST("Legacy_InjectedFromHiddenValues", graph.FindNode(8)->customEffect->injectedConstants ==
                 (InjectedConstants::InputMask | InjectedConstants::OutputSize));
            TEST("Legacy_LargeGroupIsSingleGroup", graph.FindNode(9)->customEffect->dispatchShape == DispatchShape::SingleGroup &&
                 graph.FindNode(9)->customEffect->outputExtent == OutputExtent{ L"DiagramSize" });
            TEST("Legacy_SmallGroupTiles", graph.FindNode(10)->customEffect->dispatchShape == DispatchShape::PerOutputPixel);
            TEST("Legacy_NotCompiledOnLoad", !graph.FindNode(6)->customEffect->isCompiled());

            // Dangling references are dropped and reported where they were.
            TEST("Legacy_DanglingEdgeDropped", graph.Edges().size() == 3 &&
                 graph.FindNode(10)->runtimeError.find(L"missing node 99") != std::wstring::npos);
            TEST("Legacy_DanglingBindingDropped", graph.FindNode(8)->propertyBindings.empty() &&
                 graph.FindNode(8)->runtimeError.find(L"'Split'") != std::wstring::npos);
            TEST("Legacy_SortsAfterRepair", graph.UnorderableNodes().empty());

            // nextId was below the ids in use.
            EffectNode extra;
            extra.type = NodeType::Output;
            TEST("Legacy_NextIdRepaired", graph.AddNode(std::move(extra)) == 12);

            // A re-save writes the contracts, and they read back unchanged.
            const auto again = GraphModel::FromJson(graph.ToJson());
            bool sameAll = true;
            for (const auto& node : graph.Nodes())
            {
                if (!node.customEffect.has_value()) continue;
                const auto* back = again.FindNode(node.id);
                sameAll = sameAll && back && back->customEffect.has_value() &&
                          SameContracts(*node.customEffect, *back->customEffect);
            }
            TEST("Legacy_ContractsRoundTrip", sameAll);

            // A zero extent multiplier reads as 1 and is reported.
            std::wstring edited(graph.ToJson());
            const size_t columns = edited.find(L"\"columns\":6");
            if (columns != std::wstring::npos) edited.replace(columns, 11, L"\"columns\":0");
            const auto zeroed = GraphModel::FromJson(winrt::hstring(edited));
            const auto* patch = zeroed.FindNode(7);
            TEST("Legacy_BadExtentMultiplierReported", columns != std::wstring::npos && patch &&
                 patch->customEffect->outputExtent == (OutputExtent{ L"PatchSize", 1, 4 }) &&
                 patch->runtimeError.find(L"outputExtent columns") != std::wstring::npos);
        }

        // Every committed graph fixture still loads and round-trips.
        SL_TEST_CASE(TestCommittedFixturesLoad, Tag::Fast)
        {
            std::printf("\n=== Graph load: committed fixtures ===\n");
            const auto dir = FixtureDir();
            int loaded = 0, failed = 0;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (entry.path().extension() != L".json") continue;
                if (entry.path().filename() == L"manifest.json") continue;   // the fixture list, not a graph
                try
                {
                    auto graph = GraphModel::FromJson(winrt::hstring(ReadText(entry.path())));
                    auto again = GraphModel::FromJson(graph.ToJson());
                    const bool ok = again.Nodes().size() == graph.Nodes().size() &&
                                    again.Edges().size() == graph.Edges().size() &&
                                    graph.UnorderableNodes().empty();
                    if (ok) ++loaded;
                    else { ++failed; std::printf("    %ls did not round-trip\n", entry.path().filename().c_str()); }
                }
                catch (...)
                {
                    // Counted as a failure; the name says which file.
                    ++failed;
                    std::printf("    %ls did not load\n", entry.path().filename().c_str());
                }
            }
            TEST("Fixtures_AllLoadAndRoundTrip", loaded >= 2 && failed == 0);
        }

        SL_TEST_CASE(TestLoadValidation, Tag::Fast)
        {
            std::printf("\n=== Graph load: validation ===\n");
            const wchar_t* duplicate = LR"({"nodes":[
                {"id":3,"name":"a","type":"Output","position":[0,0],"properties":[],"inputPins":[],"outputPins":[]},
                {"id":3,"name":"b","type":"Output","position":[0,0],"properties":[],"inputPins":[],"outputPins":[]}],
                "edges":[],"nextId":4})";
            bool threw = false;
            try { GraphModel::FromJson(duplicate); }
            catch (const std::runtime_error&) { threw = true; }
            TEST("Load_DuplicateIdRefused", threw);

            const wchar_t* badEnum = LR"({"nodes":[
                {"id":1,"name":"a","type":"PixelShader","position":[0,0],"properties":[],"inputPins":[],"outputPins":[],
                 "customEffect":{"shaderType":7,"hlslSource":"x","inputNames":[],"parameters":[],
                 "threadGroupX":8,"threadGroupY":8,"threadGroupZ":1,"analysisOutputType":9,"analysisOutputSize":256}}],
                "edges":[],"nextId":2})";
            const auto graph = GraphModel::FromJson(badEnum);
            const auto& node = graph.Nodes()[0];
            TEST("Load_OutOfRangeEnumsReported",
                 node.customEffect->shaderType == CustomShaderType::PixelShader &&
                 node.customEffect->analysisOutputType == AnalysisOutputType::None &&
                 node.runtimeError.find(L"shaderType 7") != std::wstring::npos &&
                 node.runtimeError.find(L"analysisOutputType 9") != std::wstring::npos);

            // The retired float-buffer type (2) loads as None without an
            // error, and analysisOutputSize is optional on load.
            const wchar_t* retired = LR"({"nodes":[
                {"id":1,"name":"a","type":"ComputeShader","position":[0,0],"properties":[],"inputPins":[],"outputPins":[],
                 "customEffect":{"shaderType":1,"hlslSource":"x","inputNames":[],"parameters":[],
                 "threadGroupX":8,"threadGroupY":8,"threadGroupZ":1,"analysisOutputType":2}}],
                "edges":[],"nextId":2})";
            const auto retiredGraph = GraphModel::FromJson(retired);
            const auto& retiredNode = retiredGraph.Nodes()[0];
            TEST("Load_RetiredFloatBufferIsNone",
                 retiredNode.customEffect->analysisOutputType == AnalysisOutputType::None &&
                 retiredNode.runtimeError.empty());
            TEST("Save_KeepsAnalysisOutputSizeForOlderReaders",
                 std::wstring(retiredGraph.ToJson()).find(L"\"analysisOutputSize\":256") != std::wstring::npos);

            // A binding cycle loads, but cannot be ordered.
            const wchar_t* cycle = LR"({"nodes":[
                {"id":1,"name":"a","type":"PixelShader","position":[0,0],"properties":[{"name":"X","type":"float","value":0}],
                 "propertyBindings":{"X":{"sources":[{"nodeId":2,"field":"Value","index":0,"comp":0}]}},
                 "inputPins":[],"outputPins":[]},
                {"id":2,"name":"b","type":"PixelShader","position":[0,0],"properties":[{"name":"X","type":"float","value":0}],
                 "propertyBindings":{"X":{"sources":[{"nodeId":1,"field":"Value","index":0,"comp":0}]}},
                 "inputPins":[],"outputPins":[]},
                {"id":3,"name":"c","type":"Output","position":[0,0],"properties":[],"inputPins":[],"outputPins":[]}],
                "edges":[],"nextId":4})";
            const auto cyclic = GraphModel::FromJson(cycle);
            auto stuck = cyclic.UnorderableNodes();
            std::sort(stuck.begin(), stuck.end());
            TEST("Cycle_UnorderableNodesNamed", stuck == std::vector<uint32_t>{ 1, 2 });
        }

        SL_TEST_CASE(TestSourceKinds, Tag::Fast)
        {
            std::printf("\n=== Source kinds ===\n");
            using Effects::SourceNodeFactory;
            TEST("SourceKind_Image", ClassifySource(SourceNodeFactory::CreateImageSourceNode(L"a.png")) == SourceKind::Image);
            TEST("SourceKind_EmptyImageIsImage", ClassifySource(SourceNodeFactory::CreateImageSourceNode(L"")) == SourceKind::Image);
            TEST("SourceKind_Video", ClassifySource(SourceNodeFactory::CreateVideoSourceNode(L"a.mp4")) == SourceKind::Video);
            TEST("SourceKind_Flood", ClassifySource(SourceNodeFactory::CreateFloodSourceNode({ 1, 1, 1, 1 })) == SourceKind::Flood);
            auto falseMarker = SourceNodeFactory::CreateVideoSourceNode(L"a.mp4");
            ShaderLab::Graph::SetNodeProperty(falseMarker, cVideoMarker, false);
            TEST("SourceKind_FalseMarkerIgnored", ClassifySource(falseMarker) == SourceKind::Image);
            EffectNode output;
            output.type = NodeType::Output;
            TEST("SourceKind_NotASource", ClassifySource(output) == SourceKind::None);
        }

        SL_TEST_CASE(TestCompileIdentity, Tag::Fast)
        {
            std::printf("\n=== Compile identity ===\n");
            const auto* desc = Effects::ShaderLabEffects::Instance().FindById(L"Luminance Statistics");
            auto builtIn = Effects::ShaderLabEffects::CreateNode(*desc);
            builtIn.name = L"Renamed";
            const auto identity = Effects::CompileIdentityOf(builtIn);
            TEST("Identity_BuiltInIgnoresNodeName",
                 identity.effectId == L"Luminance Statistics" && identity.effectVersion == desc->effectVersion);
            EffectNode authored;
            authored.name = L"Mine";
            authored.customEffect.emplace();
            const auto own = Effects::CompileIdentityOf(authored);
            TEST("Identity_AuthoredUsesName", own.effectId == L"Mine" && own.effectVersion == 1);
        }

        SL_TEST_CASE(TestGraphFormatVersion, Tag::Fast)
        {
            std::printf("\n=== Graph format version ===\n");
            GraphModel graph;
            const auto saved = WDJ::JsonObject::Parse(graph.ToJson());
            TEST("FormatVersion_Written", saved.GetNamedNumber(L"formatVersion") == cGraphFormatVersion);
            TEST("FormatVersion_EffectGraphVersionWritten",
                 saved.GetNamedString(L"effectGraphVersion") == winrt::to_hstring(EFFECTGRAPH_VERSION_STRING));
            TEST("FormatVersion_NoAppVersion", !saved.HasKey(L"appVersion"));

            auto refusal = [](const wchar_t* json) -> std::string
            {
                try { GraphModel::FromJson(json); }
                catch (const std::exception& e) { return e.what(); }
                return {};
            };
            const auto newer = refusal(LR"({"formatVersion":99,"effectGraphVersion":"9.9.9","nodes":[],"edges":[]})");
            TEST("FormatVersion_NewerRefused", newer.find("format 99") != std::string::npos &&
                 newer.find("EffectGraph 9.9.9") != std::string::npos);
            const auto legacy = refusal(LR"({"formatVersion":99,"appVersion":"1.9.0","nodes":[],"edges":[]})");
            TEST("FormatVersion_LegacyAppVersionNamed", legacy.find("ShaderLab 1.9.0") != std::string::npos);
            TEST("FormatVersion_CurrentLoads",
                 refusal(LR"({"formatVersion":2,"appVersion":"1.9.0","nodes":[],"edges":[]})").empty());
        }
    }
}
