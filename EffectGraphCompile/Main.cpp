#include "pch_engine.h"
#include "../BuiltInEffects/BuiltInEffectTable.h"
#include "../Effects/BuiltInEffects.h"
#include "../Effects/EffectBlob.h"
#include "../Effects/ShaderVariants.h"
#include "../Engine/EngineBootstrap.h"
#include "../Engine/Utf8.h"
#include "../Graph/GraphModel.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>

// EffectGraphCompile: builds an effect blob from an effect saved by
// ShaderLab's Effect Designer, for a program to register with
// EffectGraphRegisterEffect. Run at the consuming project's build
// (EffectGraph.targets does it for EffectGraphEffect items). With --builtins
// it writes a header for each of ShaderLab's built-in shader effects, every
// build in, the same effect as an .effectblob file for programs that cannot
// include a header, and EffectGraphBuiltIns.h, which includes the headers
// (EffectGraphBuiltIns.targets runs it).
namespace
{
    namespace fs = std::filesystem;
    using namespace ShaderLab;

    void PrintUsage()
    {
        std::fwprintf(stderr,
            L"Usage: EffectGraphCompile <effect.json> [--header <file.h>] [--symbol <name>]\n"
            L"                          [--out <file.bin>] [--gpu-bound <name>[,<name>...]]\n"
            L"       EffectGraphCompile --builtins <dir> [--cache <dir>]\n"
            L"  --builtins   write a header for each built-in shader effect into <dir>\n"
            L"  --cache      keep compiled builds in <dir> between runs\n"
            L"  --header     write the blob as a C++ byte array\n"
            L"  --symbol     the array's name (default: c<FileName>Effect)\n"
            L"  --out        write the blob as raw bytes\n"
            L"  --gpu-bound  parameters the program binds to an analysis field; each\n"
            L"               combination gets a GPU-served build (at most %zu)\n",
            Effects::cMaxBlobGpuParameters);
    }

    bool ReadText(const fs::path& path, std::wstring& text)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            bytes.erase(0, 3);
        text = Utf8ToWide(bytes);
        return !text.empty();
    }

    std::vector<std::wstring> SplitList(const std::wstring& list)
    {
        std::vector<std::wstring> items;
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t end = list.find(L',', start);
            auto item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!item.empty()) items.push_back(std::move(item));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return items;
    }

    // c + the file's stem in PascalCase + Effect: hdr_screenshot_tonemap.json
    // gives cHdrScreenshotTonemapEffect.
    std::string DefaultSymbol(const fs::path& input)
    {
        std::string symbol = "c";
        bool wordStart = true;
        for (wchar_t ch : input.stem().wstring())
        {
            const bool letter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
            const bool digit = ch >= L'0' && ch <= L'9';
            if (!letter && !digit) { wordStart = true; continue; }
            char c = static_cast<char>(ch);
            if (wordStart && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            symbol.push_back(c);
            wordStart = false;
        }
        return symbol + "Effect";
    }

    bool WriteFile(const fs::path& path, const void* data, size_t size)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    }

    // Leaves an unchanged file alone, so what includes it does not rebuild.
    // Written beside it and renamed, so a reader never sees half a header.
    bool WriteIfChanged(const fs::path& path, const std::string& text)
    {
        std::ifstream in(path, std::ios::binary);
        if (in && std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) == text)
            return true;
        in.close();
        const fs::path temp = path.wstring() + L".tmp";
        if (!WriteFile(temp, text.data(), text.size())) return false;
        return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    // A node as its saved JSON, with the per-instance shader id cleared.
    std::wstring NodeJson(Graph::EffectNode node)
    {
        if (node.customEffect) node.customEffect->shaderGuid = GUID_NULL;
        Graph::GraphModel graph;
        graph.AddNode(std::move(node));
        return std::wstring(graph.ToJson());
    }

    // The description a built-in's header carries: the saved graph of one
    // node, with the menu placement the graph alone does not record.
    std::wstring DescriptionOf(const Effects::ShaderLabEffectDescriptor& descriptor)
    {
        Graph::GraphModel graph;
        graph.AddNode(Effects::ShaderLabEffects::CreateNode(descriptor));
        auto root = winrt::Windows::Data::Json::JsonObject::Parse(graph.ToJson());
        root.SetNamedValue(L"category", winrt::Windows::Data::Json::JsonValue::CreateStringValue(descriptor.category));
        root.SetNamedValue(L"subcategory", winrt::Windows::Data::Json::JsonValue::CreateStringValue(descriptor.subcategory));
        return std::wstring(root.Stringify());
    }

    // Why the description would register something other than `descriptor`;
    // empty when it registers the same effect.
    std::wstring RoundTripError(const Effects::ShaderLabEffectDescriptor& descriptor, const std::wstring& description)
    {
        Effects::UserEffectLoadReport report;
        const auto parsed = Effects::ShaderLabEffects::ParseEffectsFromJson(description, descriptor.name, report);
        if (parsed.size() != 1) return L"its description does not parse back to one effect";
        const auto& back = parsed.front();
        if (back.category != descriptor.category || back.subcategory != descriptor.subcategory)
            return L"its menu placement does not survive the description";
        if (back.dataOnly != descriptor.dataOnly || back.hasImageOutput != descriptor.hasImageOutput)
            return L"its outputs do not survive the description";
        if (NodeJson(Effects::ShaderLabEffects::CreateNode(back)) != NodeJson(Effects::ShaderLabEffects::CreateNode(descriptor)))
            return L"the node it creates does not survive the description";
        return {};
    }

    int WriteBuiltIns(const fs::path& directory, const std::wstring& cacheDirectory)
    {
        const auto descriptors = Effects::BuiltIn::ShaderEffects();
        std::vector<const Effects::BuiltInEffectInfo*> listed;
        for (const auto& info : Effects::BuiltInEffectList())
            if (info.shader) listed.push_back(&info);
        if (listed.size() != descriptors.size())
        {
            std::fwprintf(stderr, L"EffectGraphCompile: the engine lists %zu built-in shader effects, the table has %zu\n",
                          listed.size(), descriptors.size());
            return 3;
        }
        for (size_t i = 0; i < descriptors.size(); ++i)
        {
            if (listed[i]->name != descriptors[i].name || listed[i]->effectId != descriptors[i].effectId)
            {
                std::fwprintf(stderr, L"EffectGraphCompile: built-in %zu is '%ls' in the table but '%ls' in the engine's list\n",
                              i, descriptors[i].name.c_str(), std::wstring(listed[i]->name).c_str());
                return 3;
            }
        }

        std::error_code ec;
        fs::create_directories(directory, ec);
        // Each effect's builds compile on its own cache's workers; effects run side by side.
        std::vector<std::future<std::wstring>> results;
        for (const auto& descriptor : descriptors)
        {
            results.push_back(std::async(std::launch::async, [&directory, &cacheDirectory, &descriptor]() -> std::wstring
            {
                const std::wstring description = DescriptionOf(descriptor);
                if (auto error = RoundTripError(descriptor, description); !error.empty())
                    return descriptor.name + L": " + error;
                Effects::EffectBlobOptions options;
                options.cacheDirectory = cacheDirectory;
                const auto node = Effects::ShaderLabEffects::CreateNode(descriptor);
                for (const auto& name : Effects::GpuBindableNames(*node.customEffect))
                    options.gpuBoundParameters.push_back(Utf8ToWide(name));
                const auto blob = Effects::BuildEffectBlob(description, descriptor.name, options);
                if (blob.builds.empty()) return descriptor.name + L": " + blob.error;
                const std::string symbol = "c" + Effects::EffectSymbol(descriptor.name) + "Effect";
                const auto text = Effects::GenerateEffectHeader(blob, symbol, L"ShaderLab's built-in '" + descriptor.name + L"'",
                                                                { .registerAtStartup = true });
                if (!WriteIfChanged(directory / Effects::EffectHeaderName(descriptor.name), text))
                    return descriptor.name + L": cannot write its header";
                // The same effect as a blob file, for programs that cannot
                // include a C++ header (EffectGraphRegisterEffect).
                std::string stem = Effects::EffectHeaderName(descriptor.name);
                stem.resize(stem.size() - std::string_view(".effect.h").size());
                const auto file = Effects::EffectBlobFile(blob);
                if (!WriteIfChanged(directory / (stem + ".effectblob"), std::string(file.begin(), file.end())))
                    return descriptor.name + L": cannot write its blob";
                std::wprintf(L"  %ls: %zu build(s), %.2f MB\n", descriptor.name.c_str(), blob.builds.size(), blob.StoredBytes() / 1e6);
                return {};
            }));
        }
        int failures = 0;
        for (auto& result : results)
        {
            if (auto error = result.get(); !error.empty())
            {
                std::fwprintf(stderr, L"EffectGraphCompile: error: %ls\n", error.c_str());
                ++failures;
            }
        }
        if (failures) return 3;

        std::string all = "// Generated by EffectGraphCompile --builtins. Do not edit.\n"
                          "// Every ShaderLab built-in shader effect; each registers itself at startup.\n"
                          "#pragma once\n";
        for (const auto& descriptor : descriptors)
            all += "#include \"" + Effects::EffectHeaderName(descriptor.name) + "\"\n";
        if (!WriteIfChanged(directory / L"EffectGraphBuiltIns.h", all))
        {
            std::fwprintf(stderr, L"EffectGraphCompile: cannot write EffectGraphBuiltIns.h\n");
            return 4;
        }
        // The build's up-to-date check: touched on every successful run.
        const std::string stamp = "ok\n";
        if (!WriteFile(directory / L"EffectGraphBuiltIns.stamp", stamp.data(), stamp.size())) return 4;
        std::wprintf(L"EffectGraphCompile: %zu built-in effects in %ls\n", descriptors.size(), directory.c_str());
        return 0;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    fs::path input, header, raw, builtIns;
    std::wstring cacheDirectory;
    std::string symbol;
    Effects::EffectBlobOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        auto value = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == L"--header") { if (auto v = value()) header = v; else { PrintUsage(); return 1; } }
        else if (arg == L"--out") { if (auto v = value()) raw = v; else { PrintUsage(); return 1; } }
        else if (arg == L"--symbol") { if (auto v = value()) symbol = WideToUtf8(v); else { PrintUsage(); return 1; } }
        else if (arg == L"--gpu-bound") { if (auto v = value()) options.gpuBoundParameters = SplitList(v); else { PrintUsage(); return 1; } }
        else if (arg == L"--builtins") { if (auto v = value()) builtIns = v; else { PrintUsage(); return 1; } }
        else if (arg == L"--cache") { if (auto v = value()) cacheDirectory = v; else { PrintUsage(); return 1; } }
        else if (!arg.starts_with(L"--") && input.empty()) input = arg;
        else { PrintUsage(); return 1; }
    }
    if (builtIns.empty() && (input.empty() || (header.empty() && raw.empty()))) { PrintUsage(); return 1; }

    winrt::init_apartment();
    // Joins the engine's worker threads on every return path.
    struct EngineServicesScope { ~EngineServicesScope() { Engine::ShutdownEngineServices(); } } engineServices;

    if (!builtIns.empty()) return WriteBuiltIns(builtIns, cacheDirectory);
    if (symbol.empty()) symbol = DefaultSymbol(input);
    options.cacheDirectory = cacheDirectory;

    std::wstring json;
    if (!ReadText(input, json))
    {
        std::fwprintf(stderr, L"EffectGraphCompile: cannot read %ls\n", input.c_str());
        return 2;
    }
    const auto blob = Effects::BuildEffectBlob(json, input.wstring(), options);
    if (blob.builds.empty())
    {
        std::fwprintf(stderr, L"%ls: error: %ls\n", input.c_str(), blob.error.c_str());
        return 3;
    }
    if (!raw.empty())
    {
        const auto file = Effects::EffectBlobFile(blob);
        if (!WriteFile(raw, file.data(), file.size()))
        {
            std::fwprintf(stderr, L"EffectGraphCompile: cannot write %ls\n", raw.c_str());
            return 4;
        }
    }
    if (!header.empty())
    {
        const auto text = Effects::GenerateEffectHeader(blob, symbol, input.filename().wstring());
        if (!WriteFile(header, text.data(), text.size()))
        {
            std::fwprintf(stderr, L"EffectGraphCompile: cannot write %ls\n", header.c_str());
            return 4;
        }
    }
    std::wprintf(L"EffectGraphCompile: %ls -> %zu effect(s), %zu build(s), %.1f MB of shader code in %.2f MB\n",
                 input.filename().c_str(), blob.effects.size(), blob.builds.size(),
                 blob.BytecodeBytes() / 1e6, blob.StoredBytes() / 1e6);
    // The macros a generated header selects with.
    for (const auto& effect : blob.effects)
    {
        std::wprintf(L"  %ls\n    %hs\n", effect.name.c_str(), Effects::EffectMacro(effect.name).c_str());
        for (const auto& [option, labels] : effect.options)
        {
            std::wprintf(L"    %hs\n", Effects::OptionMacro(effect.name, option, L"ALL").c_str());
            for (const auto& label : labels)
                std::wprintf(L"    %hs\n", Effects::OptionMacro(effect.name, option, label).c_str());
        }
        for (const auto& layout : effect.gpuLayouts)
            std::wprintf(L"    %hs\n", Effects::LayoutMacro(effect.name, layout).c_str());
    }
    return 0;
}
