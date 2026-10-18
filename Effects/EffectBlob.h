#pragma once

#include "pch_engine.h"

// Effect blobs: effects built ahead of time, inside the program that ships
// them. A blob holds the effects' description (the saved-graph JSON the
// Effect Designer writes) and the compiled bytecode of every build the
// program may ask for, so registering it compiles no HLSL. Each build is
// stored on its own, its key in the clear and its bytecode compressed, so a
// generated header can leave builds out with #if and the bytecode cache can
// decompress a build only when it is first used.
namespace ShaderLab::Effects
{
    inline constexpr uint32_t cEffectBlobFormat = 3;

    struct EffectBlobOptions
    {
        // Parameters the program binds to an analysis field (statistics into
        // a tone mapper's peak). Every combination of them gets a build with
        // those parameters served on the GPU, in every option combination,
        // beside the all-CPU builds. At most cMaxBlobGpuParameters.
        std::vector<std::wstring> gpuBoundParameters;
        // A folder that keeps compiled builds between runs, so a rebuild
        // compiles only what changed. Empty: compile everything.
        std::wstring cacheDirectory;
    };
    inline constexpr size_t cMaxBlobGpuParameters = 4;

    // One stored build and what selects it.
    struct EffectBlobBuildInfo
    {
        std::wstring effect;
        // Parameters served on the GPU, in the effect's order; empty for the
        // all-CPU layout.
        std::vector<std::wstring> gpuParameters;
        // Each option this build fixes, with the label of its value. An
        // option left out is read at run time.
        std::vector<std::pair<std::wstring, std::wstring>> options;
        std::vector<uint8_t> record;   // the stored form
        size_t bytecodeBytes{ 0 };     // before compression
        bool IsGeneric() const { return gpuParameters.empty() && options.empty(); }
    };

    struct EffectBlobEffectInfo
    {
        std::wstring name;
        std::wstring effectId;
        // Specialised options, in the effect's order, with their labels.
        std::vector<std::pair<std::wstring, std::vector<std::wstring>>> options;
        // The GPU layouts built, beside the all-CPU one.
        std::vector<std::vector<std::wstring>> gpuLayouts;
    };

    struct EffectBlobBuild
    {
        std::string description;                // UTF-8 JSON
        std::vector<EffectBlobEffectInfo> effects;
        std::vector<EffectBlobBuildInfo> builds;
        std::wstring error;                     // set, with no builds, on failure

        size_t BytecodeBytes() const
        {
            size_t total = 0;
            for (const auto& build : builds) total += build.bytecodeBytes;
            return total;
        }
        size_t StoredBytes() const
        {
            size_t total = description.size();
            for (const auto& build : builds) total += build.record.size();
            return total;
        }
    };

    // The single-file form, for EffectGraphRegisterEffect.
    std::vector<uint8_t> EffectBlobFile(const EffectBlobBuild& build);

    // Compiles every shader node in `json` into blob builds. `source` names
    // the input in errors.
    EffectBlobBuild BuildEffectBlob(std::wstring_view json, const std::wstring& source,
                                                  const EffectBlobOptions& options);

    struct EffectHeaderOptions
    {
        // The header registers its effects when the program starts
        // (EffectGraphAddEffects) instead of leaving it to the program.
        bool registerAtStartup{ false };
    };

    // The C++ header form: the description and each build as arrays, each
    // build behind an #if on the macros that select it, an
    // EFFECTGRAPH_EFFECT_SOURCE named `symbol` for EffectGraphRegisterEffects,
    // and each effect's class id (EffectClsidName) for CreateEffectById.
    std::string GenerateEffectHeader(const EffectBlobBuild& build, const std::string& symbol,
                                                   const std::wstring& sourceName,
                                                   const EffectHeaderOptions& options = {});

    // The macro that selects an effect, option value or GPU layout in a
    // generated header: EFFECTGRAPH_ plus the names, uppercased, each run of
    // anything outside [A-Z0-9] one '_' ("Off (float)" -> OFF_FLOAT).
    std::string EffectMacro(std::wstring_view effect);
    std::string OptionMacro(std::wstring_view effect, std::wstring_view option, std::wstring_view label);
    std::string LayoutMacro(std::wstring_view effect, const std::vector<std::wstring>& gpuParameters);

    enum class EffectBlobStatus
    {
        Registered,
        Corrupt,        // not a blob, truncated, or builds that match none of its effects
        Incompatible,   // built against other shared HLSL headers or cache schema
        NotRegistered,  // valid, but its effects' names or ids are taken by newer versions
        NoneIncluded,   // a header whose selection left out every effect's generic build
    };

    struct EffectBlobRegistration
    {
        EffectBlobStatus status{ EffectBlobStatus::Corrupt };
        std::vector<std::wstring> effects;   // the names registered
        std::wstring error;
    };

    // A blob as a generated header holds it.
    struct EffectBlobParts
    {
        uint32_t format{ 0 };
        uint64_t contractHash{ 0 };
        std::span<const uint8_t> description;
        std::vector<std::span<const uint8_t>> records;
    };

    // Registers the effects that have their generic build, and adds every
    // build to the bytecode cache as a prebuilt entry, decompressed on first
    // use. RegisterEffectBlob copies the blob; RegisterEffectBlobParts keeps
    // pointers into memory that must outlive the process's use of the effects
    // (a generated header's arrays). Same startup rule as LoadUserEffects.
    EffectBlobRegistration RegisterEffectBlob(std::span<const uint8_t> blob);
    EffectBlobRegistration RegisterEffectBlobParts(const EffectBlobParts& parts);

    // A generated header's parts, kept for registration when the registry is
    // first used. Only records them, so a header can call it from a static
    // initializer, under the loader lock of the module that includes it.
    void QueueEffectBlobParts(EffectBlobParts parts);
    // Registers what was queued since the last call; ShaderLabEffects::Instance
    // calls it. Cheap when nothing is queued.
    void DrainQueuedEffectBlobs();
    // Why queued headers failed to register, one line each.
    std::vector<std::wstring> QueuedEffectBlobErrors();
}
