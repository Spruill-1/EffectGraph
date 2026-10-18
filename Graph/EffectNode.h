#pragma once

#include "pch_engine.h"
#include "NodeType.h"
#include "PropertyValue.h"
#include "NodeContracts.h"

namespace ShaderLab::Graph
{
    // Describes a single input or output pin on a node.
    struct PinDescriptor
    {
        std::wstring name;
        uint32_t     index{ 0 };
    };

    // Shader type for custom effects. Saved as its number, so the values are fixed.
    enum class CustomShaderType
    {
        PixelShader        = 0,
        ComputeShader      = 1,
        D3D11ComputeShader = 2,  // Raw D3D11 compute (not D2D-tiled, full atomics/groupshared)
    };

    // Describes a user-defined parameter for a custom effect.
    struct ParameterDefinition
    {
        std::wstring name;
        std::wstring typeName;       // "float", "float2", "float3", "float4", "int", "uint", "bool"
        PropertyValue defaultValue;
        float minValue{ 0.0f };
        float maxValue{ 1.0f };
        float step{ 0.01f };

        // Named labels for uint enum values (index → display name).
        // When non-empty, the Properties panel renders a ComboBox instead of a NumberBox.
        std::vector<std::wstring> enumLabels;

        // Conditional visibility: "ParamName == value" hides this parameter
        // unless the condition is met. Empty = always visible.
        // Supports ==, !=, <, <=, >, >= with numeric comparison.
        std::wstring visibleWhen;

        // GPU-binding eligibility (opt-in). When true, this parameter's
        // HLSL declaration uses the SHADERLAB_PARAM / SHADERLAB_LOAD_PARAM
        // macros from `shaderlab_params.hlsli`, and a property binding from
        // an upstream compute node can route the value GPU to GPU. The host
        // injects `_SLPARAM_<name>_GPU=0|1|2` at compile time and binds the
        // matching resource. Default false: the parameter is cbuffer-only.
        bool gpuBindable{ false };

        // Compile one shader variant per option value (needs enumLabels, not
        // gpuBindable; declared with SHADERLAB_OPTION in shaderlab_params.hlsli).
        // A value outside the labels uses the generic build. See ShaderVariants.h.
        bool specialize{ false };
    };

    // Identifies the type of non-image data produced by analysis/compute effects.
    // Saved as its number, so the values are fixed. 2 was an untyped float
    // buffer nothing rendered; a saved 2 loads as None.
    enum class AnalysisOutputType
    {
        None        = 0,
        Histogram   = 1,  // Built-in D2D histogram (256 float bins).
        Typed       = 3,  // Custom compute: typed named fields.
    };

    // Type of a single analysis output field.
    enum class AnalysisFieldType
    {
        Float,          // 1 component, 1 pixel
        Float2,         // 2 components, 1 pixel
        Float3,         // 3 components, 1 pixel
        Float4,         // 4 components, 1 pixel
        FloatArray,     // N floats packed 4-per-pixel (ceil(N/4) pixels)
        Float2Array,    // N×float2, 1 pixel each (.xy used)
        Float3Array,    // N×float3, 1 pixel each (.xyz used)
        Float4Array,    // N×float4, 1 pixel each
    };

    // The tag a field type is saved and reported as.
    inline const wchar_t* AnalysisFieldTypeTag(AnalysisFieldType type)
    {
        switch (type)
        {
        case AnalysisFieldType::Float:       return L"float";
        case AnalysisFieldType::Float2:      return L"float2";
        case AnalysisFieldType::Float3:      return L"float3";
        case AnalysisFieldType::Float4:      return L"float4";
        case AnalysisFieldType::FloatArray:  return L"floatarray";
        case AnalysisFieldType::Float2Array: return L"float2array";
        case AnalysisFieldType::Float3Array: return L"float3array";
        case AnalysisFieldType::Float4Array: return L"float4array";
        }
        return L"unknown";
    }

    // The field type a tag names, or nullopt for an unknown tag.
    inline std::optional<AnalysisFieldType> AnalysisFieldTypeFromTag(std::wstring_view tag)
    {
        for (auto type : { AnalysisFieldType::Float, AnalysisFieldType::Float2, AnalysisFieldType::Float3,
                           AnalysisFieldType::Float4, AnalysisFieldType::FloatArray, AnalysisFieldType::Float2Array,
                           AnalysisFieldType::Float3Array, AnalysisFieldType::Float4Array })
            if (tag == AnalysisFieldTypeTag(type)) return type;
        return std::nullopt;
    }

    // Returns the number of scalar components per element for a field type.
    inline uint32_t AnalysisFieldComponentCount(AnalysisFieldType type)
    {
        switch (type)
        {
        case AnalysisFieldType::Float:       return 1;
        case AnalysisFieldType::Float2:      return 2;
        case AnalysisFieldType::Float3:      return 3;
        case AnalysisFieldType::Float4:      return 4;
        case AnalysisFieldType::FloatArray:  return 1;
        case AnalysisFieldType::Float2Array: return 2;
        case AnalysisFieldType::Float3Array: return 3;
        case AnalysisFieldType::Float4Array: return 4;
        default: return 4;
        }
    }

    // Returns true if the field type is an array type.
    inline bool AnalysisFieldIsArray(AnalysisFieldType type)
    {
        return type >= AnalysisFieldType::FloatArray;
    }

    // Returns the number of UAV pixels consumed by one field.
    inline uint32_t AnalysisFieldPixelCount(AnalysisFieldType type, uint32_t arrayLength)
    {
        switch (type)
        {
        case AnalysisFieldType::Float:
        case AnalysisFieldType::Float2:
        case AnalysisFieldType::Float3:
        case AnalysisFieldType::Float4:
            return 1;
        case AnalysisFieldType::FloatArray:
            return (arrayLength + 3) / 4;  // pack 4 floats per pixel
        case AnalysisFieldType::Float2Array:
        case AnalysisFieldType::Float3Array:
        case AnalysisFieldType::Float4Array:
            return arrayLength;  // 1 pixel per element
        default: return 1;
        }
    }

    // Describes one output field of an analysis compute shader.
    struct AnalysisFieldDescriptor
    {
        std::wstring name;
        AnalysisFieldType type{ AnalysisFieldType::Float4 };
        uint32_t arrayLength{ 0 };  // only for array types; max 4096 elements

        // GPU-binding publication (opt-in). When true, the
        // upstream effect implementing IEngineComputeOutput exposes
        // this field's slot as readable from a downstream
        // SHADERLAB_GPU_BUFFER binding. The slot index in the
        // structured-buffer SRV is the field's position in the
        // analysisFields vector, so the field's order is part of the
        // binding ABI -- reordering an existing effect's analysisFields
        // requires bumping its effectVersion.
        // Default false: the field is CPU-only.
        bool gpuPublish{ false };

        uint32_t pixelCount() const { return AnalysisFieldPixelCount(type, arrayLength); }
    };

    // Runtime value of a single analysis output field (read back from GPU).
    struct AnalysisFieldValue
    {
        std::wstring name;
        AnalysisFieldType type{ AnalysisFieldType::Float4 };

        // For scalar types (Float/Float2/Float3/Float4).
        std::array<float, 4> components{ 0, 0, 0, 0 };

        // For array types. Flat storage, stride = AnalysisFieldComponentCount(type).
        std::vector<float> arrayData;
    };

    // Unpacks typed fields from a row of float4 pixels laid out field after
    // field by AnalysisFieldPixelCount. Components past floatCount read 0.
    inline std::vector<AnalysisFieldValue> UnpackAnalysisFields(
        const std::vector<AnalysisFieldDescriptor>& fieldDescs,
        const float* float4s,
        size_t floatCount)
    {
        auto at = [&](size_t index) { return index < floatCount ? float4s[index] : 0.0f; };
        std::vector<AnalysisFieldValue> fields;
        fields.reserve(fieldDescs.size());
        size_t pixelOffset = 0;
        for (const auto& fieldDesc : fieldDescs)
        {
            AnalysisFieldValue value;
            value.name = fieldDesc.name;
            value.type = fieldDesc.type;
            const uint32_t componentCount = AnalysisFieldComponentCount(fieldDesc.type);
            if (!AnalysisFieldIsArray(fieldDesc.type))
            {
                for (uint32_t c = 0; c < componentCount; ++c)
                    value.components[c] = at(pixelOffset * 4 + c);
            }
            else if (fieldDesc.type == AnalysisFieldType::FloatArray)
            {
                // Four elements per pixel.
                value.arrayData.resize(fieldDesc.arrayLength);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                    value.arrayData[i] = at(pixelOffset * 4 + i);
            }
            else
            {
                // One element per pixel.
                value.arrayData.resize(static_cast<size_t>(fieldDesc.arrayLength) * componentCount);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                    for (uint32_t c = 0; c < componentCount; ++c)
                        value.arrayData[static_cast<size_t>(i) * componentCount + c] = at((pixelOffset + i) * 4 + c);
            }
            pixelOffset += fieldDesc.pixelCount();
            fields.push_back(std::move(value));
        }
        return fields;
    }

    // The array length a value's arrayData holds (0 for a scalar or vector).
    inline uint32_t AnalysisFieldArrayLength(const AnalysisFieldValue& value)
    {
        if (!AnalysisFieldIsArray(value.type)) return 0;
        const uint32_t componentCount = AnalysisFieldComponentCount(value.type);
        return value.type == AnalysisFieldType::FloatArray
            ? static_cast<uint32_t>(value.arrayData.size())
            : static_cast<uint32_t>(value.arrayData.size() / (std::max)(componentCount, 1u));
    }

    // The descriptors that lay values out as they are.
    inline std::vector<AnalysisFieldDescriptor> AnalysisLayoutOf(const std::vector<AnalysisFieldValue>& values)
    {
        std::vector<AnalysisFieldDescriptor> layout;
        layout.reserve(values.size());
        for (const auto& value : values)
            layout.push_back({ value.name, value.type, AnalysisFieldArrayLength(value) });
        return layout;
    }

    // Packs values into the row of float4s UnpackAnalysisFields reads.
    inline std::vector<float> PackAnalysisFields(const std::vector<AnalysisFieldValue>& values)
    {
        std::vector<float> float4s;
        for (const auto& value : values)
        {
            const size_t start = float4s.size();
            const uint32_t arrayLength = AnalysisFieldArrayLength(value);
            float4s.resize(start + static_cast<size_t>(AnalysisFieldPixelCount(value.type, arrayLength)) * 4, 0.0f);
            const uint32_t componentCount = AnalysisFieldComponentCount(value.type);
            if (!AnalysisFieldIsArray(value.type))
            {
                for (uint32_t c = 0; c < componentCount; ++c) float4s[start + c] = value.components[c];
            }
            else if (value.type == AnalysisFieldType::FloatArray)
            {
                std::copy(value.arrayData.begin(), value.arrayData.end(), float4s.begin() + start);
            }
            else
            {
                for (uint32_t i = 0; i < arrayLength; ++i)
                    for (uint32_t c = 0; c < componentCount; ++c)
                        float4s[start + i * 4 + c] = value.arrayData[static_cast<size_t>(i) * componentCount + c];
            }
        }
        return float4s;
    }

    // Complete definition of a custom effect authored in the Effect Designer.
    // Stored per-node and serialized with the graph JSON.
    struct CustomEffectDefinition
    {
        CustomShaderType shaderType{ CustomShaderType::PixelShader };
        std::wstring hlslSource;                        // Full HLSL source code.
        std::vector<std::wstring> inputNames;           // Named inputs (textures).
        // How many TRAILING inputNames are lookup tables rather than images.
        //
        // A lookup input is sampled at coordinates the shader computes, not
        // at this pixel's own position -- a gamut-boundary table indexed by
        // (hue, I), say. Two things follow, and both are wrong for it if it is
        // treated as an ordinary image input:
        //   * it must not contribute to the output rect (a 256x128 table would
        //     otherwise shape where a 2880x1800 effect draws), and
        //   * a D2D tile query must be answered with the WHOLE table, since
        //     the shader reads arbitrary texels of it from every tile.
        // Unconnected, a lookup pin is fed a 1x1 zero bitmap rather than left
        // null (D2D will not render an effect with a null input); its alpha of
        // 0 is how the shader tells "no table wired" and falls back.
        uint32_t lookupInputCount{ 0 };

        // Variadic image inputs (pixel shaders). inputNames is the maximum and
        // the D2D effect always has that many inputs; the node shows only the
        // pins in use plus one spare (GraphModel::FitVariadicPins).
        // Unconnected inputs get the 1x1 zero placeholder, and an `InputMask`
        // cbuffer field, if declared, receives the connected pins (bit i = pin i).
        bool variadicInputs{ false };
        std::vector<ParameterDefinition> parameters;    // Declared cbuffer parameters.

        // ShaderLab built-in effect identity (empty for user-authored effects).
        std::wstring shaderLabEffectId;
        uint32_t shaderLabEffectVersion{ 0 };

        // The evaluator's contract with this effect (NodeContracts.h). Built-in
        // effects declare them; a graph saved without them derives them on
        // load (DeriveContracts).
        NodeKind nodeKind{ NodeKind::Shader };
        DispatchShape dispatchShape{ DispatchShape::PerOutputPixel };
        OutputExtent outputExtent;
        InjectedConstants injectedConstants{ InjectedConstants::None };

        // Thread-group size. A D2D compute shader (ComputeShader) needs it to
        // size its dispatch; D3D11 compute reads [numthreads] from the
        // bytecode and uses these only to derive dispatchShape for old graphs.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Analysis output metadata (for compute effects).
        AnalysisOutputType analysisOutputType{ AnalysisOutputType::None };

        // Typed analysis output field descriptors.
        // The compute shader writes fields sequentially to Output row 0.
        // Each field's pixel offset = sum of prior fields' pixelCount().
        std::vector<AnalysisFieldDescriptor> analysisFields;

        // Runtime: compiled bytecode. Not serialized; the evaluator compiles
        // hlslSource, with its variant macros, the first time it needs the node.
        std::vector<uint8_t> compiledBytecode;

        // Runtime: unique shader GUID for this instance (avoids D2D shader ID collisions).
        GUID shaderGuid{};

        bool isCompiled() const { return !compiledBytecode.empty(); }

        // Total UAV pixels needed for all analysis fields.
        uint32_t totalAnalysisPixels() const
        {
            uint32_t total = 0;
            for (const auto& f : analysisFields)
                total += f.pixelCount();
            return total;
        }
    };

    // Stores non-image output data from analysis/compute effects.
    // Read back from the GPU after the effect graph is evaluated.
    struct AnalysisOutput
    {
        AnalysisOutputType type{ AnalysisOutputType::None };

        // Typed field values (for AnalysisOutputType::Typed).
        std::vector<AnalysisFieldValue> fields;

        // The D2D Histogram's output (AnalysisOutputType::Histogram).
        std::vector<float> data;         // Bin values.
        std::wstring       label;        // Human-readable description.
        uint32_t           channelIndex{ 0 }; // For per-channel data (R=0, G=1, B=2, A=3).
    };

    // Specifies the source of a single scalar value in a property binding.
    struct ComponentSource
    {
        uint32_t sourceNodeId{ 0 };
        std::wstring sourceFieldName;   // analysis field name on source node
        uint32_t sourceIndex{ 0 };      // array element index (0 for scalar fields)
        uint32_t sourceComponent{ 0 };  // 0-3 for .xyzw within that element
    };

    // Binds a downstream node's property to upstream analysis output values.
    // Supports per-component routing: each destination component can come from
    // a different source field/node/component.
    struct PropertyBinding
    {
        // Per-component sources. Size matches destination component count:
        //   float → 1, float2 → 2, float3 → 3, float4 → 4
        // Use nullopt for unbound components (keeps authored default).
        std::vector<std::optional<ComponentSource>> sources;

        // Whole-array mode: for vector<float> destination properties.
        // When set, ignores per-component sources and copies entire array.
        bool wholeArray{ false };
        uint32_t wholeArraySourceNodeId{ 0 };
        std::wstring wholeArraySourceFieldName;

        // True when pred(sourceNodeId) holds for any node this binding reads:
        // the whole-array source, or each set per-component source.
        template <class Pred>
        bool AnySourceNode(Pred&& pred) const
        {
            if (wholeArray) return pred(wholeArraySourceNodeId);
            for (const auto& source : sources)
                if (source.has_value() && pred(source->sourceNodeId)) return true;
            return false;
        }

        // Calls fn(sourceNodeId) for each node this binding reads. A node
        // feeding several components is reported once per component.
        template <class Fn>
        void ForEachSourceNode(Fn&& fn) const
        {
            AnySourceNode([&](uint32_t sourceNodeId) { fn(sourceNodeId); return false; });
        }

        bool ReadsFrom(uint32_t nodeId) const
        {
            return AnySourceNode([nodeId](uint32_t sourceNodeId) { return sourceNodeId == nodeId; });
        }
    };

    // How to read a node's `lastGpuMs`.
    //
    // A bare "no number" is ambiguous in exactly the way the dispatch counter
    // once was: a node doing no work and a node whose work is real but
    // unattributable look identical. These states say which, so the canvas can
    // report the reason instead of a dash.
    enum class GpuNodeState : uint8_t
    {
        // GPU timing is off, or this node took no part in the frame.
        Unmeasured = 0,
        // lastGpuMs is a real measurement of this node: its own compute
        // dispatch, or -- for the node at the end of a Direct2D chain -- the
        // whole chain that feeds it.
        Measured,
        // A D2D image node in the middle of a chain. Direct2D fuses the chain
        // and evaluates it lazily at DrawImage, so this node's cost is real
        // but is already counted inside a downstream chain end's figure.
        // Separating it out would mean materialising the node on its own,
        // which changes the workload being measured.
        Fused,
        // Needed this frame, but did no GPU work: a clean compute node served
        // from its cached result. Zero cost is the correct answer here, not a
        // missing measurement.
        Cached,
        // Not needed this frame, so the evaluator skipped it entirely --
        // nothing downstream consumes its output. Distinct from Cached: a
        // cached node would have run if its input had changed; this one would
        // not have run either way. Wiring or binding it is what makes it
        // measurable, and saying so beats a blank the reader has to guess at.
        Idle,
        // Produces no GPU work by construction: a parameter node (Clock, Float
        // Parameter, ...) with no image output. Zero is not an approximation
        // here -- there is no GPU work to attribute in the first place.
        CpuOnly,
    };

    // A node in the effect graph DAG.
    // Each node represents a D2D image source, a built-in D2D effect,
    // a custom pixel/compute shader, or the final output.
    struct EffectNode
    {
        uint32_t   id{ 0 };
        std::wstring name;
        NodeType   type{ NodeType::Source };

        // Canvas position for the visual graph editor (in DIPs).
        winrt::Windows::Foundation::Numerics::float2 position{ 0.0f, 0.0f };

        // Effect-specific properties (e.g., Gaussian blur StdDeviation, Flood Color).
        // These are the "authored" defaults — never mutated by bindings at runtime.
        std::map<std::wstring, PropertyValue> properties;

        // Property bindings: property name → upstream analysis field source.
        // During evaluation, bound values override authored properties.
        std::map<std::wstring, PropertyBinding> propertyBindings;

        // Pin descriptors — filled when the node type is known.
        std::vector<PinDescriptor> inputPins;
        std::vector<PinDescriptor> outputPins;

        // For built-in D2D effects: the CLSID used to create the effect.
        std::optional<GUID> effectClsid;

        // Source nodes: the image or video file (or a media:// token in a
        // packaged graph). Saved under the JSON key "shaderPath".
        std::optional<std::wstring> mediaPath;

        // For custom effects authored in the Effect Designer.
        std::optional<CustomEffectDefinition> customEffect;

        // Cached output after evaluation (non-owning, managed by the render engine).
        // Not serialized.
        ID2D1Image* cachedOutput{ nullptr };

        // Analysis/compute output data (read back after evaluation). Not serialized.
        AnalysisOutput analysisOutput;

        // Runtime error message (e.g., effect creation failure). Not serialized.
        std::wstring runtimeError;

        // The baseline shader is compiling in the background, so the node has
        // no output yet (GraphEvaluator::SetAsyncCompile). Not serialized.
        bool compilePending{ false };

        // Option variants still compiling in the background. The node renders
        // with the generic shader meanwhile. Not serialized.
        uint32_t variantsCompiling{ 0 };

        // Dirty flag -- set when properties change, cleared after evaluation.
        bool dirty{ true };

        // Clock node: time-based animation source.
        bool IsClock() const
        {
            return customEffect.has_value() && customEffect->nodeKind == NodeKind::Clock;
        }
        // A custom effect the evaluator serves itself (a parameter, Clock,
        // expression...): it has data outputs and an inline control, no shader.
        bool IsParameterNode() const
        {
            return type != NodeType::Source && customEffect.has_value() &&
                   IsParameterKind(customEffect->nodeKind);
        }
        // A shader with no image output: it exists for its analysis fields.
        bool IsDataOnlyEffect() const
        {
            return !IsParameterNode() && customEffect.has_value() &&
                   !customEffect->hlslSource.empty() && outputPins.empty();
        }
        bool isPlaying{ false };   // runtime: toggled by on-node Play/Pause
        // runtime: GPU milliseconds attributed to this node on the last
        // measured frame, or -1 when it was not measured.
        //
        // A COMPUTE node's figure is its own dispatch: exact, because each is
        // its own submission. A D2D IMAGE node has no figure of its own --
        // Direct2D fuses a chain and evaluates it lazily at DrawImage -- so
        // only the node at the END of a chain carries one, and it is the cost
        // of the whole chain feeding it. Attributing intermediates would mean
        // materialising each separately, which changes the workload being
        // measured.
        double lastGpuMs{ -1.0 };
        // runtime: why lastGpuMs looks the way it does. See GpuNodeState.
        GpuNodeState gpuState{ GpuNodeState::Unmeasured };

        double clockTime{ 0.0 };   // runtime: accumulated time in seconds
        // runtime: which UpdateRate bucket clockTime last landed in. -1 =
        // none yet. Only used when UpdateRate > 0; it is what lets the
        // clock dirty its consumers at a chosen rate instead of every
        // frame. Not serialized -- it is a tick artefact, not graph state.
        long long clockTickBucket{ -1 };

        // Set each frame by GraphModel::ComputeNeeded / MarkAllNeeded
        // (FrameExecutor): true if this node feeds something the host demands.
        // The evaluator skips nodes where needed == false.
        bool needed{ true };
    };

    // True when pred(sourceNodeId) holds for any node one of `node`'s
    // property bindings reads.
    template <class Pred>
    bool AnyBindingSource(const EffectNode& node, Pred&& pred)
    {
        for (const auto& [property, binding] : node.propertyBindings)
            if (binding.AnySourceNode(pred)) return true;
        return false;
    }

    // Calls fn(sourceNodeId) for each node `node`'s property bindings read,
    // once per binding component.
    template <class Fn>
    void ForEachBindingSource(const EffectNode& node, Fn&& fn)
    {
        for (const auto& [property, binding] : node.propertyBindings)
            binding.ForEachSourceNode(fn);
    }
}
