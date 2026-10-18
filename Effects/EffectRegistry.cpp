#include "pch_engine.h"
#include "EffectRegistry.h"

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Singleton
    // -----------------------------------------------------------------------

    EffectRegistry& EffectRegistry::Instance()
    {
        static EffectRegistry instance;
        return instance;
    }

    EffectRegistry::EffectRegistry()
    {
        RegisterBuiltInEffects();
    }

    void EffectRegistry::Register(EffectDescriptor desc)
    {
        // Every effect has at least one output pin.
        if (desc.outputPins.empty())
            desc.outputPins.push_back({ L"Output", 0 });

        m_descriptors.push_back(std::move(desc));
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    const EffectDescriptor* EffectRegistry::FindByName(std::wstring_view name) const
    {
        for (const auto& d : m_descriptors)
        {
            // Lengths are explicit: a string_view need not be null-terminated.
            if (::CompareStringOrdinal(d.name.data(), static_cast<int>(d.name.size()),
                    name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL)
                return &d;
        }
        return nullptr;
    }

    const EffectDescriptor* EffectRegistry::FindByClsid(const GUID& clsid) const
    {
        for (const auto& d : m_descriptors)
        {
            if (IsEqualGUID(d.clsid, clsid))
                return &d;
        }
        return nullptr;
    }

    std::vector<const EffectDescriptor*> EffectRegistry::ByCategory(std::wstring_view category) const
    {
        std::vector<const EffectDescriptor*> result;
        for (const auto& d : m_descriptors)
        {
            if (d.category == category)
                result.push_back(&d);
        }
        return result;
    }

    std::vector<std::wstring> EffectRegistry::Categories() const
    {
        std::set<std::wstring> unique;
        for (const auto& d : m_descriptors)
            unique.insert(d.category);
        return { unique.begin(), unique.end() };
    }

    // -----------------------------------------------------------------------
    // Node factory
    // -----------------------------------------------------------------------

    EffectNode EffectRegistry::CreateNode(const EffectDescriptor& desc)
    {
        EffectNode node;
        node.type = NodeType::BuiltInEffect;
        node.name = desc.name;
        node.effectClsid = desc.clsid;
        node.inputPins = desc.inputPins;
        node.outputPins = desc.outputPins;
        node.properties = desc.defaultProperties;
        return node;
    }

    EffectNode EffectRegistry::CreateOutputNode()
    {
        EffectNode node;
        node.type = NodeType::Output;
        node.name = L"Output";
        node.inputPins.push_back({ L"Input", 0 });
        return node;
    }

    // -----------------------------------------------------------------------
    // Built-in D2D effect catalog
    // -----------------------------------------------------------------------

    // Helper macros for common pin patterns.
    #define SINGLE_INPUT  {{ L"Input", 0 }}
    #define DUAL_INPUT    {{ L"Input", 0 }, { L"Input 2", 1 }}

    void EffectRegistry::RegisterBuiltInEffects()
    {
        // ===== Blur =====

        Register({
            .clsid = CLSID_D2D1GaussianBlur,
            .name = L"Gaussian Blur",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"StandardDeviation", 3.0f },
            },
            .propertyMetadata = {
                { L"StandardDeviation", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 250.0f, .step = 0.5f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1DirectionalBlur,
            .name = L"Directional Blur",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"StandardDeviation", 3.0f },
                { L"Angle", 0.0f },
            },
            .propertyMetadata = {
                { L"StandardDeviation", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 250.0f, .step = 0.5f } },
                { L"Angle", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 360.0f, .step = 1.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Shadow,
            .name = L"Shadow",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"BlurStandardDeviation", 3.0f },
                { L"Color", Numerics::float4{ 0.0f, 0.0f, 0.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"BlurStandardDeviation", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 250.0f, .step = 0.5f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B", L"A" } } },
            },
        });

        // ===== Color =====

        {
            // Identity color matrix.
            D2D1_MATRIX_5X4_F identity{};
            identity._11 = 1.0f; identity._22 = 1.0f; identity._33 = 1.0f; identity._44 = 1.0f;

            Register({
                .clsid = CLSID_D2D1ColorMatrix,
                .name = L"Color Matrix",
                .category = L"Color",
                .inputPins = SINGLE_INPUT,
                .defaultProperties = {
                    { L"ColorMatrix", identity },
                },
                .propertyMetadata = {
                    { L"ColorMatrix", { .uiHint = PropertyUIHint::MatrixEditor, .minValue = -10.0f, .maxValue = 10.0f, .step = 0.01f } },
                },
            });
        }

        Register({
            .clsid = CLSID_D2D1Brightness,
            .name = L"Brightness",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"WhitePoint", Numerics::float2{ 1.0f, 1.0f } },
                { L"BlackPoint", Numerics::float2{ 0.0f, 0.0f } },
            },
            .propertyMetadata = {
                { L"WhitePoint", { .uiHint = PropertyUIHint::VectorEditor, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f, .componentLabels = { L"X", L"Y" } } },
                { L"BlackPoint", { .uiHint = PropertyUIHint::VectorEditor, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f, .componentLabels = { L"X", L"Y" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Contrast,
            .name = L"Contrast",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Contrast", 0.0f },
            },
            .propertyMetadata = {
                { L"Contrast", { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Exposure,
            .name = L"Exposure",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"ExposureValue", 0.0f },
            },
            .propertyMetadata = {
                { L"ExposureValue", { .uiHint = PropertyUIHint::Slider, .minValue = -10.0f, .maxValue = 10.0f, .step = 0.1f } },
            },
        });

        // Grayscale — no editable properties.
        Register({
            .clsid = CLSID_D2D1Grayscale,
            .name = L"Grayscale",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1HueRotation,
            .name = L"Hue Rotation",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Angle", 0.0f },
            },
            .propertyMetadata = {
                { L"Angle", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 360.0f, .step = 1.0f } },
            },
        });

        // Invert — no editable properties.
        Register({
            .clsid = CLSID_D2D1Invert,
            .name = L"Invert",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Saturation,
            .name = L"Saturation",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Saturation", 0.5f },
            },
            .propertyMetadata = {
                { L"Saturation", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 2.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Sepia,
            .name = L"Sepia",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Intensity", 0.5f },
            },
            .propertyMetadata = {
                { L"Intensity", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1TemperatureTint,
            .name = L"Temperature & Tint",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Temperature", 0.0f },
                { L"Tint", 0.0f },
            },
            .propertyMetadata = {
                { L"Temperature", { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"Tint", { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Vignette,
            .name = L"Vignette",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Color", Numerics::float4{ 0.0f, 0.0f, 0.0f, 1.0f } },
                { L"Amount", 0.5f },
                { L"Size", 0.5f },
            },
            .propertyMetadata = {
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B", L"A" } } },
                { L"Amount", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"Size", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        // ===== Composition =====

        Register({
            .clsid = CLSID_D2D1Blend,
            .name = L"Blend",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Mode", static_cast<uint32_t>(D2D1_BLEND_MODE_MULTIPLY) },
            },
            .propertyMetadata = {
                { L"Mode", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = {
                    L"Multiply", L"Screen", L"Darken", L"Lighten",
                    L"Dissolve", L"Color Burn", L"Linear Burn", L"Darker Color",
                    L"Lighter Color", L"Color Dodge", L"Linear Dodge", L"Overlay",
                    L"Soft Light", L"Hard Light", L"Vivid Light", L"Linear Light",
                    L"Pin Light", L"Hard Mix", L"Difference", L"Exclusion",
                    L"Hue", L"Saturation", L"Color", L"Luminosity",
                    L"Subtract", L"Division",
                } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Composite,
            .name = L"Composite",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Mode", static_cast<uint32_t>(D2D1_COMPOSITE_MODE_SOURCE_OVER) },
            },
            .propertyMetadata = {
                { L"Mode", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = {
                    L"Source Over", L"Destination Over",
                    L"Source In", L"Destination In",
                    L"Source Out", L"Destination Out",
                    L"Source Atop", L"Destination Atop",
                    L"XOR", L"Plus",
                    L"Source Copy", L"Bounded Source Copy",
                    L"Mask Invert",
                } } },
            },
        });

        // Alpha Mask — no editable properties.
        Register({
            .clsid = CLSID_D2D1AlphaMask,
            .name = L"Alpha Mask",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Opacity,
            .name = L"Opacity",
            .category = L"Composition",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Opacity", 1.0f },
            },
            .propertyMetadata = {
                { L"Opacity", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1CrossFade,
            .name = L"Cross Fade",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Weight", 0.5f },
            },
            .propertyMetadata = {
                { L"Weight", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        // ===== Transform =====

        Register({
            .clsid = CLSID_D2D1Scale,
            .name = L"Scale",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Scale", Numerics::float2{ 1.0f, 1.0f } },
                { L"CenterPoint", Numerics::float2{ 0.0f, 0.0f } },
            },
            .propertyMetadata = {
                { L"Scale", { .uiHint = PropertyUIHint::VectorEditor, .minValue = 0.01f, .maxValue = 10.0f, .step = 0.01f, .componentLabels = { L"X", L"Y" } } },
                { L"CenterPoint", { .uiHint = PropertyUIHint::VectorEditor, .step = 1.0f, .componentLabels = { L"X", L"Y" } } },
            },
        });

        // 2D Affine Transform — 3x2 matrix doesn't fit PropertyValue variant.
        Register({
            .clsid = CLSID_D2D12DAffineTransform,
            .name = L"2D Affine Transform",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Border,
            .name = L"Border",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"EdgeModeX", static_cast<uint32_t>(D2D1_BORDER_EDGE_MODE_CLAMP) },
                { L"EdgeModeY", static_cast<uint32_t>(D2D1_BORDER_EDGE_MODE_CLAMP) },
            },
            .propertyMetadata = {
                { L"EdgeModeX", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = { L"Clamp", L"Wrap", L"Mirror" } } },
                { L"EdgeModeY", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = { L"Clamp", L"Wrap", L"Mirror" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Crop,
            .name = L"Crop",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Rect", Numerics::float4{ -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX } },
            },
            .propertyMetadata = {
                { L"Rect", { .uiHint = PropertyUIHint::VectorEditor, .step = 1.0f,
                    .componentLabels = { L"Left", L"Top", L"Right", L"Bottom" } } },
            },
        });

        // ===== Sharpen / Detail =====

        Register({
            .clsid = CLSID_D2D1Sharpen,
            .name = L"Sharpen",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Sharpness", 0.0f },
                { L"Threshold", 0.0f },
            },
            .propertyMetadata = {
                { L"Sharpness", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.1f } },
                { L"Threshold", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1EdgeDetection,
            .name = L"Edge Detection",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Strength", 0.5f },
                { L"BlurRadius", 0.0f },
            },
            .propertyMetadata = {
                { L"Strength", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"BlurRadius", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.1f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Emboss,
            .name = L"Emboss",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Height", 1.0f },
                { L"Direction", 0.0f },
            },
            .propertyMetadata = {
                { L"Height", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.1f } },
                { L"Direction", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 360.0f, .step = 1.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Posterize,
            .name = L"Posterize",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"RedValueCount", static_cast<uint32_t>(4) },
                { L"GreenValueCount", static_cast<uint32_t>(4) },
                { L"BlueValueCount", static_cast<uint32_t>(4) },
            },
            .propertyMetadata = {
                { L"RedValueCount", { .uiHint = PropertyUIHint::Slider, .minValue = 2.0f, .maxValue = 16.0f, .step = 1.0f } },
                { L"GreenValueCount", { .uiHint = PropertyUIHint::Slider, .minValue = 2.0f, .maxValue = 16.0f, .step = 1.0f } },
                { L"BlueValueCount", { .uiHint = PropertyUIHint::Slider, .minValue = 2.0f, .maxValue = 16.0f, .step = 1.0f } },
            },
        });

        // ===== Lighting =====

        Register({
            .clsid = CLSID_D2D1PointDiffuse,
            .name = L"Point Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"DiffuseConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"LightPosition", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"DiffuseConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1SpotDiffuse,
            .name = L"Spot Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"PointsAt", Numerics::float3{ 0.0f, 0.0f, 0.0f } },
                { L"Focus", 1.0f },
                { L"LimitingConeAngle", 90.0f },
                { L"DiffuseConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"LightPosition", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"PointsAt", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"Focus", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 200.0f, .step = 0.1f } },
                { L"LimitingConeAngle", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 90.0f, .step = 1.0f } },
                { L"DiffuseConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1PointSpecular,
            .name = L"Point Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"SpecularExponent", 1.0f },
                { L"SpecularConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"LightPosition", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"SpecularExponent", { .uiHint = PropertyUIHint::Slider, .minValue = 1.0f, .maxValue = 128.0f, .step = 0.5f } },
                { L"SpecularConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1SpotSpecular,
            .name = L"Spot Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"PointsAt", Numerics::float3{ 0.0f, 0.0f, 0.0f } },
                { L"Focus", 1.0f },
                { L"LimitingConeAngle", 90.0f },
                { L"SpecularExponent", 1.0f },
                { L"SpecularConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"LightPosition", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"PointsAt", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y", L"Z" } } },
                { L"Focus", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 200.0f, .step = 0.1f } },
                { L"LimitingConeAngle", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 90.0f, .step = 1.0f } },
                { L"SpecularExponent", { .uiHint = PropertyUIHint::Slider, .minValue = 1.0f, .maxValue = 128.0f, .step = 0.5f } },
                { L"SpecularConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1DistantDiffuse,
            .name = L"Distant Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Azimuth", 0.0f },
                { L"Elevation", 0.0f },
                { L"DiffuseConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"Azimuth", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 360.0f, .step = 1.0f } },
                { L"Elevation", { .uiHint = PropertyUIHint::Slider, .minValue = -90.0f, .maxValue = 90.0f, .step = 1.0f } },
                { L"DiffuseConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        Register({
            .clsid = CLSID_D2D1DistantSpecular,
            .name = L"Distant Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Azimuth", 0.0f },
                { L"Elevation", 0.0f },
                { L"SpecularExponent", 1.0f },
                { L"SpecularConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
                { L"Color", Numerics::float3{ 1.0f, 1.0f, 1.0f } },
            },
            .propertyMetadata = {
                { L"Azimuth", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 360.0f, .step = 1.0f } },
                { L"Elevation", { .uiHint = PropertyUIHint::Slider, .minValue = -90.0f, .maxValue = 90.0f, .step = 1.0f } },
                { L"SpecularExponent", { .uiHint = PropertyUIHint::Slider, .minValue = 1.0f, .maxValue = 128.0f, .step = 0.5f } },
                { L"SpecularConstant", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"SurfaceScale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 0.1f } },
                { L"Color", { .uiHint = PropertyUIHint::VectorEditor, .componentLabels = { L"R", L"G", L"B" } } },
            },
        });

        // ===== Distort =====

        Register({
            .clsid = CLSID_D2D1DisplacementMap,
            .name = L"Displacement Map",
            .category = L"Distort",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Scale", 0.0f },
            },
            .propertyMetadata = {
                { L"Scale", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 500.0f, .step = 1.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Morphology,
            .name = L"Morphology",
            .category = L"Distort",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Mode", static_cast<uint32_t>(0) },
                { L"Width", static_cast<uint32_t>(1) },
                { L"Height", static_cast<uint32_t>(1) },
            },
            .propertyMetadata = {
                { L"Mode", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = { L"Erode", L"Dilate" } } },
                { L"Width", { .uiHint = PropertyUIHint::Slider, .minValue = 1.0f, .maxValue = 100.0f, .step = 1.0f } },
                { L"Height", { .uiHint = PropertyUIHint::Slider, .minValue = 1.0f, .maxValue = 100.0f, .step = 1.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Turbulence,
            .name = L"Turbulence",
            .category = L"Source",
            .inputPins = {},          // no inputs — generates noise
            .defaultProperties = {
                { L"Offset", Numerics::float2{ 0.0f, 0.0f } },
            },
            .propertyMetadata = {
                { L"Offset", { .uiHint = PropertyUIHint::VectorEditor, .minValue = -10000.0f, .maxValue = 10000.0f, .step = 1.0f, .componentLabels = { L"X", L"Y" } } },
            },
        });

        // ===== HDR / Tone Map =====

        Register({
            .clsid = CLSID_D2D1HdrToneMap,
            .name = L"HDR Tone Map",
            .category = L"HDR",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"InputMaxLuminance", 4000.0f },
                { L"OutputMaxLuminance", 300.0f },
            },
            .propertyMetadata = {
                { L"InputMaxLuminance", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 10.0f } },
                { L"OutputMaxLuminance", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 10.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1WhiteLevelAdjustment,
            .name = L"White Level Adjustment",
            .category = L"HDR",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"InputWhiteLevel", 80.0f },
                { L"OutputWhiteLevel", 80.0f },
            },
            .propertyMetadata = {
                { L"InputWhiteLevel", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 10.0f } },
                { L"OutputWhiteLevel", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10000.0f, .step = 10.0f } },
            },
        });

        // ===== Other =====

        Register({
            .clsid = CLSID_D2D1Histogram,
            .name = L"Histogram",
            .category = L"Analysis",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"ChannelSelect", static_cast<uint32_t>(0) },
                { L"NumBins", static_cast<uint32_t>(256) },
            },
            .propertyMetadata = {
                { L"ChannelSelect", { .uiHint = PropertyUIHint::ComboBox, .enumLabels = { L"Red", L"Green", L"Blue", L"Alpha" } } },
                { L"NumBins", { .uiHint = PropertyUIHint::Slider, .minValue = 2.0f, .maxValue = 1024.0f, .step = 1.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1Tile,
            .name = L"Tile",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Rect", Numerics::float4{ 0.0f, 0.0f, 100.0f, 100.0f } },
            },
            .propertyMetadata = {
                { L"Rect", { .uiHint = PropertyUIHint::VectorEditor, .step = 1.0f,
                    .componentLabels = { L"Left", L"Top", L"Right", L"Bottom" } } },
            },
        });

        // Premultiply — no editable properties.
        Register({
            .clsid = CLSID_D2D1Premultiply,
            .name = L"Premultiply",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        // Unpremultiply — no editable properties.
        Register({
            .clsid = CLSID_D2D1UnPremultiply,
            .name = L"Unpremultiply",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        {
            // Identity LUT: 256 linearly-spaced values from 0.0 to 1.0.
            std::vector<float> identityLut(256);
            for (int i = 0; i < 256; ++i)
                identityLut[i] = static_cast<float>(i) / 255.0f;

            Register({
                .clsid = CLSID_D2D1TableTransfer,
                .name = L"Table Transfer",
                .category = L"Color",
                .inputPins = SINGLE_INPUT,
                .defaultProperties = {
                    { L"RedTable",   identityLut },
                    { L"GreenTable", identityLut },
                    { L"BlueTable",  identityLut },
                    { L"AlphaTable", identityLut },
                    { L"RedDisable",   false },
                    { L"GreenDisable", false },
                    { L"BlueDisable",  false },
                    { L"AlphaDisable", false },
                },
                .propertyMetadata = {
                    { L"RedTable",   { .uiHint = PropertyUIHint::CurveEditor } },
                    { L"GreenTable", { .uiHint = PropertyUIHint::CurveEditor } },
                    { L"BlueTable",  { .uiHint = PropertyUIHint::CurveEditor } },
                    { L"AlphaTable", { .uiHint = PropertyUIHint::CurveEditor } },
                    { L"RedDisable",   { .uiHint = PropertyUIHint::Checkbox } },
                    { L"GreenDisable", { .uiHint = PropertyUIHint::Checkbox } },
                    { L"BlueDisable",  { .uiHint = PropertyUIHint::Checkbox } },
                    { L"AlphaDisable", { .uiHint = PropertyUIHint::Checkbox } },
                },
            });
        }

        Register({
            .clsid = CLSID_D2D1GammaTransfer,
            .name = L"Gamma Transfer",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"RedAmplitude", 1.0f },
                { L"RedExponent", 1.0f },
                { L"RedOffset", 0.0f },
                { L"RedDisable", false },
                { L"GreenAmplitude", 1.0f },
                { L"GreenExponent", 1.0f },
                { L"GreenOffset", 0.0f },
                { L"GreenDisable", false },
                { L"BlueAmplitude", 1.0f },
                { L"BlueExponent", 1.0f },
                { L"BlueOffset", 0.0f },
                { L"BlueDisable", false },
                { L"AlphaAmplitude", 1.0f },
                { L"AlphaExponent", 1.0f },
                { L"AlphaOffset", 0.0f },
                { L"AlphaDisable", false },
            },
            .propertyMetadata = {
                { L"RedAmplitude",   { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"RedExponent",    { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"RedOffset",      { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"RedDisable",     { .uiHint = PropertyUIHint::Checkbox } },
                { L"GreenAmplitude", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"GreenExponent",  { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"GreenOffset",    { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"GreenDisable",   { .uiHint = PropertyUIHint::Checkbox } },
                { L"BlueAmplitude",  { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"BlueExponent",   { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"BlueOffset",     { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"BlueDisable",    { .uiHint = PropertyUIHint::Checkbox } },
                { L"AlphaAmplitude", { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"AlphaExponent",  { .uiHint = PropertyUIHint::Slider, .minValue = 0.0f, .maxValue = 10.0f, .step = 0.01f } },
                { L"AlphaOffset",    { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"AlphaDisable",   { .uiHint = PropertyUIHint::Checkbox } },
            },
        });

        // Convolve Matrix — kernel matrix doesn't fit PropertyValue variant.
        Register({
            .clsid = CLSID_D2D1ConvolveMatrix,
            .name = L"Convolve Matrix",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Straighten,
            .name = L"Straighten",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Angle", 0.0f },
            },
            .propertyMetadata = {
                { L"Angle", { .uiHint = PropertyUIHint::Slider, .minValue = -45.0f, .maxValue = 45.0f, .step = 0.5f } },
            },
        });

        Register({
            .clsid = CLSID_D2D1HighlightsShadows,
            .name = L"Highlights & Shadows",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Highlights", 0.0f },
                { L"Shadows", 0.0f },
            },
            .propertyMetadata = {
                { L"Highlights", { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
                { L"Shadows", { .uiHint = PropertyUIHint::Slider, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.01f } },
            },
        });

        // 3D Lookup Table — requires a LUT resource, not a simple property.
        Register({
            .clsid = CLSID_D2D1LookupTable3D,
            .name = L"3D Lookup Table",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });
    }

    #undef SINGLE_INPUT
    #undef DUAL_INPUT
}
