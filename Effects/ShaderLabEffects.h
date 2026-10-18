#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Effects
{
    // Describes a pre-built ShaderLab effect with embedded HLSL.
    struct ShaderLabEffectDescriptor
    {
        std::wstring name;
        std::wstring category;      // "Analysis", "Source", "Parameter"
        std::wstring subcategory;   // Optional second-tier grouping under
                                    // category (e.g. "Highlights", "Scopes",
                                    // "Gamut Mapping"). Empty = ungrouped,
                                    // shown directly under the category.
        Graph::CustomShaderType shaderType{ Graph::CustomShaderType::PixelShader };

        // Stable identifier and version for upgrade detection.
        std::wstring effectId;          // Stable ID (survives renames)
        // Bump when a change should reach saved graphs (they keep their own
        // HLSL copy); TestEffectVersionGuard flags changes and says how. A
        // ColorMath change reaches graphs whose HLSL #includes the library;
        // bump the effects whose output it changes, for graphs holding an
        // inline copy.
        uint32_t effectVersion{ 1 };

        // Embedded HLSL source — compiled on first use.
        std::string hlslSource;

        // Named texture inputs.
        std::vector<std::wstring> inputNames;

        // Declared parameters with defaults + UI metadata.
        std::vector<Graph::ParameterDefinition> parameters;

        // Analysis output fields (for compute shaders).
        std::vector<Graph::AnalysisFieldDescriptor> analysisFields;
        Graph::AnalysisOutputType analysisOutputType{ Graph::AnalysisOutputType::None };

        // The evaluator's contract with the effect; see NodeContracts.h. A
        // D3D11 compute effect's thread-group size is its HLSL [numthreads].
        Graph::NodeKind nodeKind{ Graph::NodeKind::Shader };
        Graph::DispatchShape dispatchShape{ Graph::DispatchShape::PerOutputPixel };
        Graph::OutputExtent outputExtent;
        Graph::InjectedConstants injectedConstants{ Graph::InjectedConstants::None };

        // A D2D compute shader's thread-group size (user effects only).
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Hidden default properties (in cbuffer but not shown in Properties panel).
        // Set on node creation. The ones injectedConstants names are written
        // by the evaluator from then on.
        std::map<std::wstring, Graph::PropertyValue> hiddenDefaults;
        // Trailing inputNames that are lookup tables. See
        // CustomEffectDefinition::lookupInputCount.
        uint32_t lookupInputCount{ 0 };

        // inputNames is the maximum; the node shows the pins in use plus one.
        // See CustomEffectDefinition::variadicInputs.
        bool variadicInputs{ false };

        // Host-computed cbuffer contents ("derived constants"). For values
        // that depend only on the node's parameters but are expensive --
        // a gamut boundary polygon, a fit scale per intensity -- which a
        // pixel shader would otherwise rebuild for every pixel. Called by the
        // evaluator whenever it packs the node's cbuffer; `write(name, data,
        // floatCount)` fills the cbuffer variable of that name if, and only
        // if, the compiled shader declares it, so a saved graph carrying an
        // older HLSL revision is simply unaffected. Implementations should
        // memoize on their inputs: a node is re-applied every frame its
        // upstream content changes.
        using DerivedConstantWriter =
            std::function<void(std::wstring_view name, const float* data, size_t floatCount)>;
        using DerivedConstantsFn = std::function<void(
            const std::map<std::wstring, Graph::PropertyValue>& props,
            const DerivedConstantWriter& write)>;
        DerivedConstantsFn deriveConstants;

        // Image-output size of a D3D11 compute effect that depends on its
        // inputs' sizes. `inputSizes` is indexed by pin, {0, 0} for an
        // unwired pin. Returning {0, 0} falls back to the evaluator's usual
        // sizing (OutputWidth/OutputHeight, then input 0). A size past the
        // D3D11 texture limit is refused with a node error.
        using ImageOutputSizeFn = std::function<D2D1_SIZE_U(
            const std::map<std::wstring, Graph::PropertyValue>& props,
            const std::vector<D2D1_SIZE_U>& inputSizes)>;
        ImageOutputSizeFn deriveImageOutputSize;

        // Data-only effects have no visible image output pin. They produce
        // analysis output fields but their image output is internal only.
        bool dataOnly{ false };

        // Image-producing compute effects output a texture (not just analysis data).
        // Creates an output pin so downstream pixel shaders can read the result.
        bool hasImageOutput{ false };

        // File a user effect was loaded from; empty for built-ins.
        std::wstring sourcePath;
        bool IsUserEffect() const { return !sourcePath.empty(); }
    };

    // Result of LoadUserEffects. Every file that did not load has an entry
    // in `errors`.
    struct UserEffectLoadReport
    {
        std::vector<std::wstring> directories;  // every directory scanned
        std::vector<std::wstring> loaded;       // effect names now registered
        std::vector<std::wstring> errors;       // "<file>: <reason>"
    };

    // What to do when a user effect's id or name matches one already
    // registered.
    enum class UserEffectConflict
    {
        Reject,     // keep the existing effect; report the incoming one as an error
        Replace,    // the incoming definition takes the existing one's place
    };

    // Registry of all ShaderLab pre-built effects, plus any user effects
    // registered from outside the build (LoadUserEffects).
    class ShaderLabEffects
    {
    public:
        static ShaderLabEffects& Instance();

        const ShaderLabEffectDescriptor* FindByName(std::wstring_view name) const;
        const ShaderLabEffectDescriptor* FindById(std::wstring_view effectId) const;
        const std::vector<ShaderLabEffectDescriptor>& All() const { return m_effects; }

        // Sum of all built-in effect versions (user effects excluded). Any
        // bump raises it; equal sums do not imply identical catalogs.
        uint32_t LibraryVersion() const
        {
            uint32_t version = 0;
            for (const auto& effect : m_effects)
                if (!effect.IsUserEffect()) version += effect.effectVersion;
            return version;
        }
        std::vector<const ShaderLabEffectDescriptor*> ByCategory(std::wstring_view category) const;
        std::vector<std::wstring> Categories() const;

        // Create a fully-configured EffectNode from a descriptor.
        static Graph::EffectNode CreateNode(const ShaderLabEffectDescriptor& desc);

        // User effects
        // Register every shader node in each saved graph (*.json) in
        // `directory` as an effect. An optional top-level "category" picks the
        // menu group (default "User"). Effects that fail to compile are
        // reported, not added. A missing directory is not an error.
        // Call at startup, before any descriptor pointer is held: this appends
        // to the registry and may replace entries in place.
        const UserEffectLoadReport& LoadUserEffects(const std::wstring& directory);
        const UserEffectLoadReport& UserEffectReport() const { return m_userReport; }

        // The descriptor each shader node in one saved graph's JSON would be
        // registered with, registering nothing. Failures go to `report`.
        static std::vector<ShaderLabEffectDescriptor> ParseEffectsFromJson(
            std::wstring_view json, const std::wstring& source, UserEffectLoadReport& report);

        // Registers every shader node in one saved graph's JSON as an effect,
        // as LoadUserEffects does for each file. `source` names it in errors
        // and becomes each descriptor's sourcePath. With `compileCheck` an
        // effect must compile first; an effect blob brings its own bytecode.
        // Returns the names registered. `accept`, when set, picks which of the
        // JSON's effects to register; `adopt`, when set, may change each one
        // first (an effect blob makes its built-ins built-in). A built-in that
        // is already registered at the same version counts as registered.
        // Same startup rule as LoadUserEffects.
        std::vector<std::wstring> RegisterEffectsFromJson(
            std::wstring_view json, const std::wstring& source, bool compileCheck, UserEffectLoadReport& report,
            const std::function<bool(const ShaderLabEffectDescriptor&)>& accept = {},
            const std::function<void(ShaderLabEffectDescriptor&)>& adopt = {});

        // %LOCALAPPDATA%\ShaderLab\effects, or empty if it cannot be resolved.
        static std::wstring DefaultUserEffectsDirectory();

    private:
        ShaderLabEffects();
        void RegisterAlwaysAvailable();

        std::vector<ShaderLabEffectDescriptor> m_effects;
        UserEffectLoadReport m_userReport;
    };

    // The C++ hooks a built-in effect carries beyond its HLSL, looked up by
    // the descriptor id a node's CustomEffectDefinition records. The evaluator
    // reaches them through this interface rather than the catalog singleton.
    // Returned pointers stay valid while the provider is unchanged.
    class IEffectHooks
    {
    public:
        virtual ~IEffectHooks() = default;
        virtual const ShaderLabEffectDescriptor::DerivedConstantsFn*
            DerivedConstants(std::wstring_view effectId) const = 0;
        virtual const ShaderLabEffectDescriptor::ImageOutputSizeFn*
            ImageOutputSize(std::wstring_view effectId) const = 0;
    };

    // Hooks from ShaderLabEffects::Instance(), the evaluator's default.
    const IEffectHooks& BuiltInEffectHooks();

    // The conflict policy LoadUserEffects applies; public so tests can call it.
    UserEffectConflict ResolveUserEffectConflict(
        const ShaderLabEffectDescriptor& existing,
        const ShaderLabEffectDescriptor& incoming);

    // Shared HLSL color math functions. Shaders get them with
    // #include "shaderlab_colormath.hlsli"; the text has an include guard.
    inline constexpr const char* cColorMathIncludeName = "shaderlab_colormath.hlsli";
    const std::string& GetColorMathHLSL();

    // Gamut boundary search and the ICtCp Gamut Boundary LUT's geometry,
    // stamp and reader, as #include "shaderlab_gamut.hlsli". It includes the
    // color math itself and has an include guard.
    inline constexpr const char* cGamutIncludeName = "shaderlab_gamut.hlsli";
    const std::string& GetGamutHLSL();
}
