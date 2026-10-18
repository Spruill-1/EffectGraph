#pragma once

#include "pch_engine.h"
#include "CustomEffectBase.h"

namespace ShaderLab::Effects
{
    // Custom D2D effect that runs a user-supplied compute shader.
    //
    // Implements both ID2D1EffectImpl (effect lifecycle) and
    // ID2D1ComputeTransform (GPU compute shader execution via UAV).
    //
    // The compute transform differs from the draw transform:
    //   - Uses a compute shader (cs_5_0) instead of a pixel shader
    //   - Writes to a UAV output buffer instead of a render target
    //   - Must specify thread group dimensions for dispatch
    //
    // Host-managed state (outside the D2D property system): the shader
    // bytecode and constant buffer (CustomEffectBase) and the thread group
    // size. The constant buffer is uploaded per tile in CalculateThreadgroups,
    // with the tile's origin written into its first 8 bytes (_TileOffset).
    class CustomComputeShaderEffect
        : public CustomEffectBase<CustomComputeShaderEffect, ID2D1ComputeTransform>
    {
    public:
        static constexpr const wchar_t* cDisplayName = L"Custom Compute Shader";
        static constexpr const wchar_t* cDescription = L"Runs a user-supplied compute shader.";

        // CLSID for this custom effect.
        // {B2C3D4E5-2345-6789-ABCD-EF0123456789}
        static constexpr GUID CLSID_CustomComputeShader =
        { 0xb2c3d4e5, 0x2345, 0x6789, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

        // Registers CLSID_CustomComputeShader with one input. Call once at startup.
        static HRESULT RegisterEffect(ID2D1Factory1* factory)
        {
            return RegisterWithInputCount(factory, CLSID_CustomComputeShader, 1);
        }

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(
            ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;

        // ---- ID2D1ComputeTransform ----
        IFACEMETHODIMP SetComputeInfo(ID2D1ComputeInfo* computeInfo) override;
        IFACEMETHODIMP CalculateThreadgroups(
            const D2D1_RECT_L* outputRect,
            UINT32* dimensionX,
            UINT32* dimensionY,
            UINT32* dimensionZ) override;

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
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex,
            D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;

        // ---- Host-facing API (managed outside D2D property system) ----

        // Uploads the constant buffer now, so a property edit reaches D2D
        // before the next draw. CalculateThreadgroups uploads it again per
        // tile with that tile's _TileOffset, which is the copy the shader sees.
        HRESULT ForceUploadConstantBuffer();

        // The shader's [numthreads] x and y; the dispatch covers the output
        // tile in groups of that size. D2D tiles in 2D, so z is always one group.
        void SetThreadGroupSize(UINT32 x, UINT32 y);

    private:
        friend class CustomEffectBase<CustomComputeShaderEffect, ID2D1ComputeTransform>;
        CustomComputeShaderEffect() = default;
        ~CustomComputeShaderEffect() = default;

        winrt::com_ptr<ID2D1ComputeInfo> m_computeInfo;
        D2D1_RECT_L       m_inputRect{};  // Stored from MapInputRectsToOutputRect.

        // Thread group size for the dispatch (SetThreadGroupSize).
        UINT32 m_threadGroupX{ 8 };
        UINT32 m_threadGroupY{ 8 };

        // Whether a shader has ever been successfully loaded.
        bool m_shaderLoaded{ false };
    };
}
