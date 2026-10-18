#pragma once

#include "pch_engine.h"
#include "CustomEffectBase.h"

namespace ShaderLab::Effects
{
    // Custom D2D effect that runs a user-supplied pixel shader.
    //
    // Implements both ID2D1EffectImpl (effect lifecycle) and
    // ID2D1DrawTransform (GPU pixel shader execution).
    //
    // The effect is registered with D2D via RegisterEffect() using a
    // well-known CLSID. Each instance can load different HLSL bytecode
    // at runtime (hot-reload). Constant buffer values are populated from
    // the EffectNode property map.
    //
    // Host-managed state (outside the D2D property system): the shader
    // bytecode and constant buffer (CustomEffectBase), the fixed output size
    // of a source effect, and how many inputs are images.
    //
    // Lifecycle:
    //   1. The evaluator creates it with Create, which returns the impl.
    //   2. It loads bytecode (LoadShaderBytecode) and the cbuffer
    //      (SetConstantBufferData).
    //   3. D2D calls PrepareForRender, which loads the shader and uploads
    //      the cbuffer; ForceLoadShader / ForceUploadConstantBuffer do it at
    //      once for a node dirty every frame.
    class CustomPixelShaderEffect
        : public CustomEffectBase<CustomPixelShaderEffect, ID2D1DrawTransform>
    {
    public:
        static constexpr const wchar_t* cDisplayName = L"Custom Pixel Shader";
        static constexpr const wchar_t* cDescription = L"Runs a user-supplied pixel shader.";

        // CLSID for this custom effect.
        // {A1B2C3D4-1234-5678-9ABC-DEF012345678}
        static constexpr GUID CLSID_CustomPixelShader =
        { 0xa1b2c3d4, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78 } };

        // Registers CLSID_CustomPixelShader with one input. Call once at startup.
        static HRESULT RegisterEffect(ID2D1Factory1* factory)
        {
            return RegisterWithInputCount(factory, CLSID_CustomPixelShader, 1);
        }

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(
            ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;

        // ---- ID2D1DrawTransform ----
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override;

        // ---- ID2D1Transform ----
        IFACEMETHODIMP MapInputRectsToOutputRect(
            const D2D1_RECT_L* inputRects,
            const D2D1_RECT_L* inputOpaqueSubRects,
            UINT32 inputRectCount,
            D2D1_RECT_L* outputRect,
            D2D1_RECT_L* outputOpaqueSubRect) override;
        IFACEMETHODIMP MapOutputRectToInputRects(
            const D2D1_RECT_L* outputRect,
            D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const override;
        // Kept only so the previous whole-input behaviour stays readable and
        // revertible while the sub-rect answer is on trial. Not an override.
        IFACEMETHODIMP MapOutputRectToInputRectsWholeInput(
            const D2D1_RECT_L* outputRect,
            D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const;
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex,
            D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;

        // ---- Host-facing API (managed outside D2D property system) ----

        // How many input pins are real IMAGE inputs, as opposed to pins
        // reserved to carry analysis textures for gpu-bindable parameters.
        // Output bounds must come from the image inputs ONLY: an analysis
        // texture is 1 x N texels and says nothing about where this effect
        // draws, so unioning it into the output rect collapses it and the
        // node renders nothing at all -- with no error anywhere. Defaults to
        // "all pins are image inputs", which is correct for every effect that
        // declares no gpu-bindable parameters.
        void SetImageInputCount(UINT32 n) { m_imageInputCount = n; }
        UINT32 ImageInputCount() const
        {
            return (m_imageInputCount == 0 || m_imageInputCount > m_inputCount)
                 ? m_inputCount : m_imageInputCount;
        }

        // Force-upload the constant buffer to the GPU via DrawInfo.
        // Call this from the evaluator when properties change, bypassing
        // D2D's PrepareForRender change detection.
        HRESULT ForceUploadConstantBuffer();

        // Load bytecode set by LoadShaderBytecode now, not at the next
        // PrepareForRender. D2D calls PrepareForRender only after a property
        // change, so a node dirty every frame would keep its old shader while
        // ForceUploadConstantBuffer sends it the new shader's cbuffer layout.
        HRESULT ForceLoadShader();

        // Answer tile queries for image inputs with the tile itself (true,
        // the default) or with the whole image. Set by the evaluator from
        // EvaluatorOptions::subRectInputDemand.
        void SetSubRectInputDemand(bool on) { m_subRectInputDemand = on; }

        // Check if shader bytecode needs initial loading.
        bool NeedsShaderLoad() const { return m_shaderBytecode.empty(); }

        // Set fixed output size for source effects (no inputs).
        // When set, MapInputRectsToOutputRect uses this instead of requiring inputs.
        void SetFixedOutputSize(UINT32 width, UINT32 height)
        {
            m_fixedOutputWidth = width;
            m_fixedOutputHeight = height;
        }

    private:
        friend class CustomEffectBase<CustomPixelShaderEffect, ID2D1DrawTransform>;
        CustomPixelShaderEffect() = default;
        ~CustomPixelShaderEffect() = default;

        winrt::com_ptr<ID2D1DrawInfo>      m_drawInfo;

        UINT32            m_imageInputCount{ 0 };  // 0 = same as m_inputCount
        // Every input's own rect, from the last MapInputRectsToOutputRect.
        std::vector<D2D1_RECT_L> m_allInputRects;
        D2D1_RECT_L       m_inputRect{};
        D2D1_RECT_L       m_lastOutputRect{}; // From MapInputRectsToOutputRect, for 1:1 TEXCOORD mapping.

        bool              m_subRectInputDemand{ true };

        // Fixed output size for zero-input source effects.
        UINT32 m_fixedOutputWidth{ 0 };
        UINT32 m_fixedOutputHeight{ 0 };
    };
}
