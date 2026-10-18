#include "pch_engine.h"
#include "ShaderLabEffects.h"
#include "ShaderCompiler.h"
#include "ShaderVariants.h"
#include "BytecodeCache.h"
#include "BuiltInEffects.h"
#include "../Engine/Utf8.h"
#include "../Graph/GraphModel.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <shlobj.h>

// User effects: ShaderLab effects registered from files outside the build.
// A user effect file is a saved graph. Each shader node's embedded definition
// is registered like a built-in: an Add Effect menu entry, graph_add_node by
// name, and the Update Effect prompt for graphs holding an older copy.

namespace ShaderLab::Effects
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace WDJ = winrt::Windows::Data::Json;

        bool ReadUtf8File(const fs::path& path, std::wstring& out, std::wstring& error)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) { error = L"cannot open"; return false; }
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
                bytes.erase(0, 3);
            if (bytes.empty()) { error = L"file is empty"; return false; }
            int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (wideLength <= 0) { error = L"not valid UTF-8"; return false; }
            out.resize(static_cast<size_t>(wideLength));
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), wideLength);
            return true;
        }

        // Compile the baseline exactly as the evaluator requests it. Graph
        // loading compiles without the variant macros, so its result cannot
        // validate an effect that uses shaderlab_params.hlsli.
        bool CompilesLikeTheEvaluator(const Graph::EffectNode& node, std::wstring& error)
        {
            const auto& def = *node.customEffect;
            const auto identity = CompileIdentityOf(node);
            auto result = BytecodeCache::Instance().GetOrCompile(MakeVariantRequest(
                def, identity.effectId, identity.effectVersion, ShaderTargetFor(def), 0, 0));
            if (result.status == BytecodeStatus::Ready) return true;
            error = result.errorMessage;
            return false;
        }

        std::wstring Widen(const char* text)
        {
            std::string narrow(text ? text : "");
            return std::wstring(narrow.begin(), narrow.end());
        }

        ShaderLabEffectDescriptor DescriptorFromNode(const Graph::EffectNode& node,
                                                     const std::wstring& category,
                                                     const std::wstring& subcategory,
                                                     const fs::path& file)
        {
            const auto& def = *node.customEffect;
            ShaderLabEffectDescriptor descriptor;
            descriptor.name = node.name;
            descriptor.category = category;
            descriptor.subcategory = subcategory;
            descriptor.shaderType = def.shaderType;
            descriptor.effectId = def.shaderLabEffectId.empty() ? node.name : def.shaderLabEffectId;
            descriptor.effectVersion = (std::max)(def.shaderLabEffectVersion, 1u);
            descriptor.hlslSource = WideToUtf8(def.hlslSource);
            descriptor.inputNames = def.inputNames;
            descriptor.lookupInputCount = def.lookupInputCount;
            descriptor.variadicInputs = def.variadicInputs;
            descriptor.parameters = def.parameters;
            descriptor.analysisFields = def.analysisFields;
            descriptor.analysisOutputType = def.analysisOutputType;
            descriptor.threadGroupX = def.threadGroupX;
            descriptor.threadGroupY = def.threadGroupY;
            descriptor.threadGroupZ = def.threadGroupZ;
            descriptor.nodeKind = def.nodeKind;
            descriptor.dispatchShape = def.dispatchShape;
            descriptor.outputExtent = def.outputExtent;
            descriptor.injectedConstants = def.injectedConstants;
            // Read back off the node what CreateNode derives these from.
            descriptor.dataOnly = node.outputPins.empty();
            // Both compute shader kinds can produce an image.
            descriptor.hasImageOutput = (def.shaderType == Graph::CustomShaderType::ComputeShader ||
                                         def.shaderType == Graph::CustomShaderType::D3D11ComputeShader) &&
                                        !node.outputPins.empty();
            // Properties that are not declared parameters are hidden cbuffer
            // values, such as a compute node's output size.
            for (const auto& [key, value] : node.properties)
            {
                bool declared = std::any_of(descriptor.parameters.begin(), descriptor.parameters.end(),
                    [&](const Graph::ParameterDefinition& param) { return param.name == key; });
                if (!declared) descriptor.hiddenDefaults[key] = value;
            }
            descriptor.sourcePath = file.wstring();
            return descriptor;
        }
    }

    UserEffectConflict ResolveUserEffectConflict(
        const ShaderLabEffectDescriptor& existing,
        const ShaderLabEffectDescriptor& incoming)
    {
        // `incoming` shares an id or name with `existing`. A built-in is never
        // replaced, since that would change what saved graphs upgrade to and
        // drop built-in hooks such as deriveConstants. Between user effects
        // the higher effectVersion wins; a tie keeps the file loaded first.
        if (!existing.IsUserEffect())
            return UserEffectConflict::Reject;
        return incoming.effectVersion > existing.effectVersion
            ? UserEffectConflict::Replace
            : UserEffectConflict::Reject;
    }

    std::wstring ShaderLabEffects::DefaultUserEffectsDirectory()
    {
        wchar_t* localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
            return {};
        std::wstring dir = std::wstring(localAppData) + L"\\ShaderLab\\effects";
        ::CoTaskMemFree(localAppData);
        return dir;
    }

    const UserEffectLoadReport& ShaderLabEffects::LoadUserEffects(const std::wstring& directory)
    {
        auto& report = m_userReport;
        report.directories.push_back(directory);

        std::error_code errorCode;
        if (directory.empty() || !fs::is_directory(directory, errorCode))
            return report;   // no user effects on this machine: not an error

        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(directory, errorCode))
        {
            if (!entry.is_regular_file()) continue;
            if (_wcsicmp(entry.path().extension().c_str(), L".json") == 0)
                files.push_back(entry.path());
        }
        if (errorCode)
        {
            report.errors.push_back(directory + L": cannot list directory (" + Widen(errorCode.message().c_str()) + L")");
            return report;
        }
        // Sort so conflict resolution does not depend on directory order.
        std::sort(files.begin(), files.end());

        for (const auto& file : files)
        {
            std::wstring text, readError;
            if (!ReadUtf8File(file, text, readError))
            {
                report.errors.push_back(file.filename().wstring() + L": " + readError);
                continue;
            }
            RegisterEffectsFromJson(text, file.wstring(), /*compileCheck*/ true, report);
        }
        return report;
    }

    std::vector<ShaderLabEffectDescriptor> ShaderLabEffects::ParseEffectsFromJson(
        std::wstring_view json, const std::wstring& source, UserEffectLoadReport& report)
    {
        std::vector<ShaderLabEffectDescriptor> descriptors;
        const std::wstring label = fs::path(source).filename().wstring();
        auto fail = [&](const std::wstring& why) { report.errors.push_back(label + L": " + why); };

        Graph::GraphModel graph;
        std::wstring category = L"User", subcategory;
        try
        {
            const winrt::hstring text(json);
            graph = Graph::GraphModel::FromJson(text);
            auto root = WDJ::JsonObject::Parse(text);
            if (root.HasKey(L"category"))
            {
                std::wstring fileCategory(root.GetNamedString(L"category"));
                if (!fileCategory.empty()) category = fileCategory;
            }
            if (root.HasKey(L"subcategory"))
                subcategory = root.GetNamedString(L"subcategory");
        }
        catch (winrt::hresult_error const& error) { fail(L"not a ShaderLab graph (" + std::wstring(error.message()) + L")"); return descriptors; }
        catch (std::exception const& error)       { fail(Widen(error.what())); return descriptors; }

        for (const auto& node : graph.Nodes())
            if (node.customEffect.has_value())   // not sources, D2D built-ins, outputs
                descriptors.push_back(DescriptorFromNode(node, category, subcategory, fs::path(source)));
        if (descriptors.empty()) fail(L"contains no shader nodes");
        return descriptors;
    }

    std::vector<std::wstring> ShaderLabEffects::RegisterEffectsFromJson(
        std::wstring_view json, const std::wstring& source, bool compileCheck, UserEffectLoadReport& report,
        const std::function<bool(const ShaderLabEffectDescriptor&)>& accept,
        const std::function<void(ShaderLabEffectDescriptor&)>& adopt)
    {
        std::vector<std::wstring> registered;
        bool addedBuiltIn = false;
        const std::wstring label = fs::path(source).filename().wstring();
        auto fail = [&](const std::wstring& why) { report.errors.push_back(label + L": " + why); };

        for (auto& incoming : ParseEffectsFromJson(json, source, report))
        {
            if (accept && !accept(incoming)) continue;
            if (adopt) adopt(incoming);
            std::wstring compileError;
            if (compileCheck && !CompilesLikeTheEvaluator(CreateNode(incoming), compileError))
            {
                // Report only the first line of the compiler log.
                auto lineEnd = compileError.find_first_of(L"\r\n");
                fail(L"'" + incoming.name + L"' does not compile: " + compileError.substr(0, lineEnd));
                continue;
            }

            // A taken id or name is a conflict: ids key the upgrade path,
            // names key the menu and graph_add_node.
            auto existing = std::find_if(m_effects.begin(), m_effects.end(),
                [&](const ShaderLabEffectDescriptor& effect)
                {
                    return effect.effectId == incoming.effectId ||
                           _wcsicmp(effect.name.c_str(), incoming.name.c_str()) == 0;
                });
            if (existing != m_effects.end())
            {
                // Two modules of one program including the same header.
                if (!existing->IsUserEffect() && !incoming.IsUserEffect() &&
                    existing->effectId == incoming.effectId && existing->effectVersion == incoming.effectVersion)
                {
                    registered.push_back(incoming.name);
                    continue;
                }
                if (ResolveUserEffectConflict(*existing, incoming) == UserEffectConflict::Reject)
                {
                    if (!existing->IsUserEffect())
                        fail(L"'" + incoming.name + L"' has the id or name of the built-in '" +
                             existing->name + L"', and built-ins are never replaced; not loaded");
                    else
                        fail(L"'" + incoming.name + L"' v" + std::to_wstring(incoming.effectVersion) +
                             L" is not newer than v" + std::to_wstring(existing->effectVersion) + L" from " +
                             fs::path(existing->sourcePath).filename().wstring() + L"; not loaded");
                    continue;
                }
                report.loaded.push_back(incoming.name);
                registered.push_back(incoming.name);
                *existing = std::move(incoming);   // in place: pointers into m_effects stay valid
                continue;
            }
            report.loaded.push_back(incoming.name);
            registered.push_back(incoming.name);
            addedBuiltIn |= !incoming.IsUserEffect();
            m_effects.push_back(std::move(incoming));
        }
        // Built-ins keep the engine's order wherever they were registered
        // from; user effects follow in the order they arrived.
        if (addedBuiltIn)
        {
            std::ranges::stable_sort(m_effects, {}, [](const ShaderLabEffectDescriptor& effect)
                { return effect.IsUserEffect() ? SIZE_MAX : BuiltInEffectOrder(effect.effectId); });
        }
        return registered;
    }
}
