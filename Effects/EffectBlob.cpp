#include "pch_engine.h"
#include "EffectBlob.h"
#include "BytecodeCache.h"
#include "BuiltInEffects.h"
#include "ShaderLabEffects.h"
#include "ShaderVariants.h"
#include "../Engine/Utf8.h"

#include <compressapi.h>

// The file form, little-endian:
//   "EGFX" | u32 format | u64 contract (IncludeLibraryHash) | u32 jsonBytes |
//   UTF-8 JSON | u32 buildCount | (u32 recordBytes | record) * buildCount
// A record is a build's compile key in the clear (u64 sourceHash,
// u64 paramSignatureHash, u64 includeLibraryHash, u32 macroBitset,
// u64 optionKey, u16-length entry point, u16-length target), then u32
// bytecode bytes, u32 compressed bytes and the LZMS-compressed bytecode. A
// generated header holds the same description and records as arrays.
//
// Each build is compressed on its own. LZMS gains nothing across builds
// (204 tone-mapper builds: 3.37 MB each alone, 3.34 MB as one stream, from
// 46.7 MB), so storing them apart costs nothing and lets a header leave
// builds out and the cache decompress only what is used.
namespace ShaderLab::Effects
{
    namespace
    {
        constexpr char cMagic[4] = { 'E', 'G', 'F', 'X' };
        // The source the registry and errors name a blob's effects by.
        const wchar_t* const cBlobSource = L"effect blob";
        // How long the builder waits for one queued compile before compiling
        // it itself.
        constexpr uint32_t cCompileWaitMs = 10 * 60 * 1000;
        // The most bytecode one build may claim, so a damaged size cannot ask
        // for an arbitrary allocation.
        constexpr uint32_t cMaxBuildBytes = 64u << 20;

        class Writer
        {
        public:
            template <typename T> void Pod(const T& value)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_bytes.insert(m_bytes.end(), bytes, bytes + sizeof(T));
            }
            void Bytes(const void* data, size_t size)
            {
                const auto* bytes = static_cast<const uint8_t*>(data);
                m_bytes.insert(m_bytes.end(), bytes, bytes + size);
            }
            void ShortString(const std::string& text)
            {
                Pod(static_cast<uint16_t>(text.size()));
                Bytes(text.data(), text.size());
            }
            std::vector<uint8_t>& Result() { return m_bytes; }

        private:
            std::vector<uint8_t> m_bytes;
        };

        class Reader
        {
        public:
            explicit Reader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}
            template <typename T> bool Pod(T& value)
            {
                if (m_bytes.size() - m_offset < sizeof(T)) return false;
                std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
                m_offset += sizeof(T);
                return true;
            }
            bool Bytes(size_t size, std::span<const uint8_t>& out)
            {
                if (m_bytes.size() - m_offset < size) return false;
                out = m_bytes.subspan(m_offset, size);
                m_offset += size;
                return true;
            }
            bool ShortString(std::string& text)
            {
                uint16_t size = 0;
                std::span<const uint8_t> bytes;
                if (!Pod(size) || !Bytes(size, bytes)) return false;
                text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return true;
            }
            bool AtEnd() const { return m_offset == m_bytes.size(); }

        private:
            std::span<const uint8_t> m_bytes;
            size_t m_offset{ 0 };
        };

        // LZMS from the Windows Compression API, in every Windows since 8:
        // a blob needs no library of its own to read.
        std::optional<std::vector<uint8_t>> Pack(std::span<const uint8_t> raw)
        {
            COMPRESSOR_HANDLE compressor = nullptr;
            if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) return std::nullopt;
            SIZE_T needed = 0;
            ::Compress(compressor, raw.data(), raw.size(), nullptr, 0, &needed);
            std::vector<uint8_t> packed(needed);
            const bool ok = needed > 0 &&
                ::Compress(compressor, raw.data(), raw.size(), packed.data(), packed.size(), &needed);
            CloseCompressor(compressor);
            if (!ok) return std::nullopt;
            packed.resize(needed);
            return packed;
        }

        std::vector<uint8_t> Unpack(std::span<const uint8_t> packed, uint32_t rawSize)
        {
            DECOMPRESSOR_HANDLE decompressor = nullptr;
            if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) return {};
            std::vector<uint8_t> raw(rawSize);
            SIZE_T produced = 0;
            const bool ok = ::Decompress(decompressor, packed.data(), packed.size(), raw.data(), raw.size(), &produced);
            CloseDecompressor(decompressor);
            if (!ok || produced != rawSize) return {};
            return raw;
        }

        struct Record
        {
            BytecodeCompileKey key;
            uint32_t bytecodeBytes{ 0 };
            std::span<const uint8_t> packed;
        };

        std::vector<uint8_t> WriteRecord(const BytecodeCompileKey& key, const std::vector<uint8_t>& bytecode,
                                         const std::vector<uint8_t>& packed)
        {
            Writer writer;
            writer.Pod(key.sourceHash);
            writer.Pod(key.paramSignatureHash);
            writer.Pod(key.includeLibraryHash);
            writer.Pod(key.macroBitset);
            writer.Pod(key.optionKey);
            writer.ShortString(key.entryPoint);
            writer.ShortString(key.target);
            writer.Pod(static_cast<uint32_t>(bytecode.size()));
            writer.Pod(static_cast<uint32_t>(packed.size()));
            writer.Bytes(packed.data(), packed.size());
            return std::move(writer.Result());
        }

        bool ReadRecord(std::span<const uint8_t> bytes, Record& record)
        {
            Reader reader(bytes);
            uint32_t packedBytes = 0;
            return reader.Pod(record.key.sourceHash) && reader.Pod(record.key.paramSignatureHash) &&
                   reader.Pod(record.key.includeLibraryHash) && reader.Pod(record.key.macroBitset) &&
                   reader.Pod(record.key.optionKey) && reader.ShortString(record.key.entryPoint) &&
                   reader.ShortString(record.key.target) && reader.Pod(record.bytecodeBytes) &&
                   reader.Pod(packedBytes) && reader.Bytes(packedBytes, record.packed) && reader.AtEnd() &&
                   record.bytecodeBytes > 0 && record.bytecodeBytes <= cMaxBuildBytes;
        }

        // The request the evaluator makes for (bits, options) of the node
        // CreateEffect makes from this descriptor, so the keys match.
        BytecodeCompileRequest RequestFor(const ShaderLabEffectDescriptor& descriptor, uint32_t bits, uint64_t options)
        {
            const auto node = ShaderLabEffects::CreateNode(descriptor);
            const auto& def = *node.customEffect;
            const auto identity = CompileIdentityOf(node);
            return MakeVariantRequest(def, identity.effectId, identity.effectVersion, ShaderTargetFor(def), bits, options);
        }

        std::wstring FirstLine(const std::wstring& text)
        {
            return text.substr(0, text.find_first_of(L"\r\n"));
        }

        // Uppercased, each run of anything outside [A-Z0-9] one '_', none at
        // either end: "HDR Screenshot Tonemap (8bpc)" -> HDR_SCREENSHOT_TONEMAP_8BPC.
        std::string MacroPart(std::wstring_view text)
        {
            std::string part;
            bool gap = false;
            for (wchar_t ch : text)
            {
                char c = 0;
                if (ch >= L'a' && ch <= L'z') c = static_cast<char>(ch - L'a' + 'A');
                else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) c = static_cast<char>(ch);
                if (!c) { gap = true; continue; }
                if (gap && !part.empty()) part.push_back('_');
                gap = false;
                part.push_back(c);
            }
            return part;
        }

        // The registration both forms share. `owner`, when set, keeps the
        // memory the records point into alive for the lazy loads.
        EffectBlobRegistration Register(const EffectBlobParts& parts, std::shared_ptr<const std::vector<uint8_t>> owner)
        {
            EffectBlobRegistration result;
            auto corrupt = [&](std::wstring why)
            {
                result.status = EffectBlobStatus::Corrupt;
                result.error = std::move(why);
                return result;
            };
            if (parts.format != cEffectBlobFormat)
                return corrupt(std::format(L"blob format {} is not {}", parts.format, cEffectBlobFormat));
            if (parts.contractHash != IncludeLibraryHash())
            {
                result.status = EffectBlobStatus::Incompatible;
                result.error = L"built against other shared HLSL headers or cache schema; rebuild it with this engine";
                return result;
            }

            std::vector<Record> records(parts.records.size());
            for (size_t i = 0; i < records.size(); ++i)
                if (!ReadRecord(parts.records[i], records[i]))
                    return corrupt(std::format(L"build {} is damaged", i));

            const std::wstring json = Utf8ToWide(std::string_view(
                reinterpret_cast<const char*>(parts.description.data()), parts.description.size()));
            UserEffectLoadReport report;
            const auto descriptors = ShaderLabEffects::ParseEffectsFromJson(json, cBlobSource, report);
            if (descriptors.empty()) return corrupt(report.errors.empty() ? L"no effects" : report.errors.front());

            // An effect is registered when its generic build is here; every
            // build must belong to one of the described effects.
            std::vector<uint64_t> sources, included;
            std::vector<std::wstring> includedNames;
            for (const auto& descriptor : descriptors)
            {
                const auto generic = RequestFor(descriptor, 0, 0).key;
                sources.push_back(generic.sourceHash);
                if (std::ranges::any_of(records, [&](const Record& r) { return r.key == generic; }))
                {
                    included.push_back(generic.sourceHash);
                    includedNames.push_back(descriptor.name);
                }
            }
            for (const auto& record : records)
                if (std::ranges::find(sources, record.key.sourceHash) == sources.end())
                    return corrupt(L"a build belongs to none of the blob's effects");
            if (includedNames.empty())
            {
                result.status = EffectBlobStatus::NoneIncluded;
                result.error = L"no effect has its generic build included";
                return result;
            }

            // Builds first, so registering never compiles.
            auto& cache = BytecodeCache::Instance();
            for (const auto& record : records)
            {
                if (std::ranges::find(included, record.key.sourceHash) == included.end()) continue;
                cache.AddPrebuilt(record.key, [owner, packed = record.packed, size = record.bytecodeBytes]
                {
                    return Unpack(packed, size);
                });
            }
            // A built-in's blob makes it built-in: no source file, and its C++ hooks.
            auto adopt = [](ShaderLabEffectDescriptor& d)
            {
                const auto* info = FindBuiltInEffect(d.effectId);
                if (!info || !info->shader || info->effectId != d.effectId || info->name != d.name) return;
                d.sourcePath.clear();
                AttachBuiltInHooks(d);
            };
            result.effects = ShaderLabEffects::Instance().RegisterEffectsFromJson(
                json, cBlobSource, /*compileCheck*/ false, report,
                [&](const ShaderLabEffectDescriptor& d) { return std::ranges::find(includedNames, d.name) != includedNames.end(); },
                adopt);
            if (result.effects.empty())
            {
                result.status = EffectBlobStatus::NotRegistered;
                result.error = report.errors.empty() ? L"nothing registered" : report.errors.front();
                return result;
            }
            result.status = EffectBlobStatus::Registered;
            if (!report.errors.empty()) result.error = report.errors.front();
            return result;
        }
    }

    std::string EffectMacro(std::wstring_view effect)
    {
        return "EFFECTGRAPH_" + MacroPart(effect);
    }

    std::string OptionMacro(std::wstring_view effect, std::wstring_view option, std::wstring_view label)
    {
        return EffectMacro(effect) + "_" + MacroPart(option) + "_" + MacroPart(label);
    }

    std::string LayoutMacro(std::wstring_view effect, const std::vector<std::wstring>& gpuParameters)
    {
        std::string macro = EffectMacro(effect) + "_GPU";
        for (const auto& parameter : gpuParameters) macro += "_" + MacroPart(parameter);
        return macro;
    }

    EffectBlobBuild BuildEffectBlob(std::wstring_view json, const std::wstring& source, const EffectBlobOptions& options)
    {
        EffectBlobBuild result;
        if (options.gpuBoundParameters.size() > cMaxBlobGpuParameters)
        {
            result.error = std::format(L"at most {} GPU-bound parameters", cMaxBlobGpuParameters);
            return result;
        }
        UserEffectLoadReport report;
        const auto descriptors = ShaderLabEffects::ParseEffectsFromJson(json, source, report);
        if (descriptors.empty())
        {
            result.error = report.errors.empty() ? L"no effects" : report.errors.front();
            return result;
        }

        // A private cache: the engine's own compile path, without touching the
        // process cache. Its disk layer is the caller's folder, when given.
        BytecodeCache cache;
        if (!options.cacheDirectory.empty()) cache.SetDiskCacheRoot(options.cacheDirectory);
        std::vector<std::pair<EffectBlobBuildInfo, BytecodeCompileRequest>> pending;
        std::vector<bool> parameterUsed(options.gpuBoundParameters.size(), false);
        for (const auto& descriptor : descriptors)
        {
            const auto node = ShaderLabEffects::CreateNode(descriptor);
            const auto& def = *node.customEffect;
            EffectBlobEffectInfo effect{ descriptor.name, descriptor.effectId };

            // The listed parameters this effect can serve on the GPU.
            const auto gpuNames = GpuBindableNames(def);
            const GpuBindingMode mode = ShaderTargetFor(def).starts_with("ps")
                ? GpuBindingMode::Texture : GpuBindingMode::StructuredBuffer;
            std::vector<size_t> bindable;
            for (size_t index = 0; index < gpuNames.size() && index < cMaxGpuBindableParams; ++index)
            {
                for (size_t listed = 0; listed < options.gpuBoundParameters.size(); ++listed)
                {
                    if (gpuNames[index] == WideToUtf8(options.gpuBoundParameters[listed]))
                    {
                        bindable.push_back(index);
                        parameterUsed[listed] = true;
                    }
                }
            }

            // The specialised options, in option-key order, with their labels.
            const auto optionNames = SpecializedOptionNames(def);
            for (const auto& optionName : optionNames)
            {
                const std::wstring wideName = Utf8ToWide(optionName);
                auto parameter = std::ranges::find_if(def.parameters,
                    [&](const Graph::ParameterDefinition& p) { return p.name == wideName; });
                effect.options.push_back({ wideName,
                    parameter != def.parameters.end() ? parameter->enumLabels : std::vector<std::wstring>{} });
            }
            std::vector<uint64_t> optionKeys{ 0 };
            for (uint64_t key : AllOptionKeys(def)) optionKeys.push_back(key);

            // Every subset of the bindable parameters, all-CPU first.
            for (uint32_t subset = 0; subset < (1u << bindable.size()); ++subset)
            {
                uint32_t bits = 0;
                std::vector<std::wstring> layout;
                for (size_t i = 0; i < bindable.size(); ++i)
                {
                    if (!(subset & (1u << i))) continue;
                    bits = WithBindingMode(bits, bindable[i], mode);
                    layout.push_back(Utf8ToWide(gpuNames[bindable[i]]));
                }
                if (!layout.empty()) effect.gpuLayouts.push_back(layout);
                for (uint64_t optionKey : optionKeys)
                {
                    EffectBlobBuildInfo info{ descriptor.name, layout };
                    for (size_t i = 0; i < effect.options.size(); ++i)
                    {
                        const uint32_t slot = OptionKeySlot(optionKey, i);
                        if (slot == 0) continue;
                        const auto& [name, labels] = effect.options[i];
                        info.options.push_back({ name, slot - 1 < labels.size() ? labels[slot - 1] : std::to_wstring(slot - 1) });
                    }
                    auto request = RequestFor(descriptor, bits, optionKey);
                    request.allowPrebuiltCompile = true;
                    pending.push_back({ std::move(info), std::move(request) });
                }
            }
            result.effects.push_back(std::move(effect));
        }
        for (size_t listed = 0; listed < parameterUsed.size(); ++listed)
        {
            if (!parameterUsed[listed])
            {
                result.error = L"no effect has a GPU-bindable parameter '" + options.gpuBoundParameters[listed] + L"'";
                result.effects.clear();
                return result;
            }
        }

        // Queued first so the cache's workers compile in parallel; a large
        // effect's variants take tens of seconds each.
        for (const auto& [info, request] : pending)
            cache.RequestCompile(request);
        for (auto& [info, request] : pending)
        {
            const auto key = request.key;
            auto compiled = cache.GetOrCompile(std::move(request), cCompileWaitMs);
            if (compiled.status != BytecodeStatus::Ready)
            {
                result.error = L"'" + info.effect + L"' does not compile: " + FirstLine(compiled.errorMessage);
                result.effects.clear();
                result.builds.clear();
                return result;
            }
            auto packed = Pack(compiled.bytecode);
            if (!packed)
            {
                result.error = L"compressing a build failed";
                result.effects.clear();
                result.builds.clear();
                return result;
            }
            info.bytecodeBytes = compiled.bytecode.size();
            info.record = WriteRecord(key, compiled.bytecode, *packed);
            result.builds.push_back(std::move(info));
        }
        result.description = WideToUtf8(json);
        return result;
    }

    std::vector<uint8_t> EffectBlobFile(const EffectBlobBuild& build)
    {
        Writer writer;
        writer.Bytes(cMagic, sizeof(cMagic));
        writer.Pod(cEffectBlobFormat);
        writer.Pod(IncludeLibraryHash());
        writer.Pod(static_cast<uint32_t>(build.description.size()));
        writer.Bytes(build.description.data(), build.description.size());
        writer.Pod(static_cast<uint32_t>(build.builds.size()));
        for (const auto& info : build.builds)
        {
            writer.Pod(static_cast<uint32_t>(info.record.size()));
            writer.Bytes(info.record.data(), info.record.size());
        }
        return std::move(writer.Result());
    }

    std::string GenerateEffectHeader(const EffectBlobBuild& build, const std::string& symbol, const std::wstring& sourceName,
                                     const EffectHeaderOptions& options)
    {
        auto bytes = [](std::string& out, std::span<const uint8_t> data)
        {
            for (size_t i = 0; i < data.size(); ++i)
            {
                if (i % 40 == 0) out += "    ";
                out += std::to_string(data[i]);
                out += (i % 40 == 39 || i + 1 == data.size()) ? ",\n" : ",";
            }
        };
        // The #if a build sits behind.
        auto condition = [](const EffectBlobBuildInfo& info)
        {
            std::string selected = "defined(" + EffectMacro(info.effect) + ")";
            if (!info.gpuParameters.empty())
                selected += " && defined(" + LayoutMacro(info.effect, info.gpuParameters) + ")";
            for (const auto& [option, label] : info.options)
            {
                selected += " && (defined(" + OptionMacro(info.effect, option, label) + ") || defined(" +
                            OptionMacro(info.effect, option, L"ALL") + "))";
            }
            return "#if !defined(EFFECTGRAPH_COMPILE_ONLY_REFERENCED) || (" + selected + ")\n";
        };

        std::string out = "// Generated by EffectGraphCompile from " + WideToUtf8(sourceName) + ". Do not edit.\n//\n";
        out += std::format("// {} effect(s), {} builds: {:.1f} MB of shader code stored in {:.1f} MB.\n",
                           build.effects.size(), build.builds.size(), build.BytecodeBytes() / 1e6, build.StoredBytes() / 1e6);
        out += "// Every build is compiled in unless EFFECTGRAPH_COMPILE_ONLY_REFERENCED is\n"
               "// defined before this header. Then a build is in when its effect's macro, its\n"
               "// GPU layout's macro (the all-CPU layout needs none) and, for each option it\n"
               "// fixes, that value's or the option's _ALL macro are defined. An option with\n"
               "// none of its macros defined is read at run time, and the effect's generic\n"
               "// build is always in.\n//\n";
        for (const auto& effect : build.effects)
        {
            out += "// " + WideToUtf8(effect.name) + "\n";
            out += "//   " + EffectMacro(effect.name) + "\n";
            for (const auto& [option, labels] : effect.options)
            {
                out += "//   " + OptionMacro(effect.name, option, L"ALL") + "\n";
                for (const auto& label : labels)
                    out += "//   " + OptionMacro(effect.name, option, label) + "\n";
            }
            for (const auto& layout : effect.gpuLayouts)
                out += "//   " + LayoutMacro(effect.name, layout) + "\n";
        }
        if (options.registerAtStartup)
            out += "//\n// Registers itself when the program starts; define\n"
                   "// EFFECTGRAPH_NO_STARTUP_REGISTRATION to call EffectGraphRegisterEffects(&" + symbol + ") instead.\n";
        else
            out += "//\n// Register with EffectGraphRegisterEffects(&" + symbol + ").\n";
        out += "#pragma once\n#include \"EffectGraph.h\"\n\n";

        out += "inline constexpr unsigned char " + symbol + "_Description[] = {\n";
        bytes(out, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(build.description.data()), build.description.size()));
        out += "};\n\n";

        for (size_t i = 0; i < build.builds.size(); ++i)
        {
            const auto& info = build.builds[i];
            out += condition(info);
            out += std::format("#define {}_HAS_{} 1\ninline constexpr unsigned char {}_Build{}[] = {{\n", symbol, i, symbol, i);
            bytes(out, info.record);
            out += "};\n#endif\n";
        }

        out += "\ninline constexpr EFFECTGRAPH_BUILD " + symbol + "_Builds[] = {\n";
        for (size_t i = 0; i < build.builds.size(); ++i)
            out += std::format("#ifdef {0}_HAS_{1}\n    {{ {0}_Build{1}, sizeof({0}_Build{1}) }},\n#endif\n", symbol, i);
        out += "    { nullptr, 0 },   // end\n};\n\n";
        out += "inline constexpr EFFECTGRAPH_EFFECT_SOURCE " + symbol + " = {\n";
        out += std::format("    {}, 0x{:016x}ull,\n", cEffectBlobFormat, IncludeLibraryHash());
        out += "    " + symbol + "_Description, sizeof(" + symbol + "_Description),\n";
        out += "    " + symbol + "_Builds, sizeof(" + symbol + "_Builds) / sizeof(" + symbol + "_Builds[0]) - 1,\n};\n";

        // Each effect's class id, for CreateEffectById.
        out += "\n";
        for (const auto& effect : build.effects)
        {
            const GUID id = EffectClsid(effect.effectId);
            out += std::format("inline constexpr GUID {} = {{ 0x{:08x}, 0x{:04x}, 0x{:04x}, "
                               "{{ 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x} }} }};\n",
                               EffectClsidName(effect.name), id.Data1, id.Data2, id.Data3,
                               id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
                               id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
        }
        if (options.registerAtStartup)
        {
            out += "\n#ifndef EFFECTGRAPH_NO_STARTUP_REGISTRATION\n"
                   "inline const HRESULT " + symbol + "_Added = EffectGraphAddEffects(&" + symbol + ");\n#endif\n";
        }
        return out;
    }

    EffectBlobRegistration RegisterEffectBlob(std::span<const uint8_t> blob)
    {
        auto owner = std::make_shared<const std::vector<uint8_t>>(blob.begin(), blob.end());
        Reader reader(*owner);
        EffectBlobParts parts;
        char magic[4]{};
        uint32_t jsonSize = 0, buildCount = 0;
        EffectBlobRegistration corrupt;
        if (!reader.Pod(magic) || std::memcmp(magic, cMagic, sizeof(cMagic)) != 0)
        {
            corrupt.error = L"not an effect blob";
            return corrupt;
        }
        if (!reader.Pod(parts.format) || !reader.Pod(parts.contractHash) || !reader.Pod(jsonSize) ||
            !reader.Bytes(jsonSize, parts.description) || !reader.Pod(buildCount))
        {
            corrupt.error = L"truncated header";
            return corrupt;
        }
        for (uint32_t i = 0; i < buildCount; ++i)
        {
            uint32_t size = 0;
            std::span<const uint8_t> record;
            if (!reader.Pod(size) || !reader.Bytes(size, record))
            {
                corrupt.error = std::format(L"build {} is truncated", i);
                return corrupt;
            }
            parts.records.push_back(record);
        }
        if (!reader.AtEnd())
        {
            corrupt.error = L"trailing bytes";
            return corrupt;
        }
        return Register(parts, std::move(owner));
    }

    EffectBlobRegistration RegisterEffectBlobParts(const EffectBlobParts& parts)
    {
        return Register(parts, nullptr);
    }

    namespace
    {
        struct BlobQueue
        {
            std::mutex mutex;
            std::vector<EffectBlobParts> parts;
            std::vector<std::wstring> errors;
            std::atomic<size_t> drained{ 0 };
            std::atomic<size_t> queued{ 0 };
            std::recursive_mutex drainMutex;
        };

        BlobQueue& Queue()
        {
            static BlobQueue sQueue;
            return sQueue;
        }

        thread_local bool tDraining = false;
    }

    void QueueEffectBlobParts(EffectBlobParts parts)
    {
        auto& queue = Queue();
        std::scoped_lock lock(queue.mutex);
        queue.parts.push_back(std::move(parts));
        queue.queued.store(queue.parts.size(), std::memory_order_release);
    }

    void DrainQueuedEffectBlobs()
    {
        auto& queue = Queue();
        // Registering reaches the registry, which calls back here.
        if (tDraining) return;
        if (queue.queued.load(std::memory_order_acquire) == queue.drained) return;
        std::scoped_lock drainLock(queue.drainMutex);
        struct Draining
        {
            Draining() { tDraining = true; }
            ~Draining() { tDraining = false; }
        } draining;
        for (;;)
        {
            EffectBlobParts next;
            {
                std::scoped_lock lock(queue.mutex);
                if (queue.drained == queue.parts.size()) break;
                next = queue.parts[queue.drained++];
            }
            const auto registration = Register(next, nullptr);
            {
                std::scoped_lock lock(queue.mutex);
                if (registration.status != EffectBlobStatus::Registered || !registration.error.empty())
                {
                    queue.errors.push_back(registration.error.empty() ? L"an effect header registered nothing"
                                                                      : registration.error);
                    std::fwprintf(stderr, L"[ShaderLab] effect header: %ls\n", queue.errors.back().c_str());
                }
            }
        }
    }

    std::vector<std::wstring> QueuedEffectBlobErrors()
    {
        auto& queue = Queue();
        std::scoped_lock lock(queue.mutex);
        return queue.errors;
    }
}
