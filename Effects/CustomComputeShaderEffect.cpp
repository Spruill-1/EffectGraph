#include "pch_engine.h"
#include "CustomComputeShaderEffect.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);

        // Check compute shader support (feature level 11_0+ required).
        if (!effectContext->IsShaderLoaded(CLSID_CustomComputeShader))
        {
            D2D1_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hardwareOptions{};
            HRESULT hr = effectContext->CheckFeatureSupport(
                D2D1_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                &hardwareOptions,
                sizeof(hardwareOptions));

            if (FAILED(hr) || !hardwareOptions.computeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
            {
                // Compute shaders not supported on this hardware.
                return D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES;
            }
        }

        return AttachToGraph(transformGraph);
    }

    IFACEMETHODIMP CustomComputeShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_computeInfo)
            return E_FAIL;

        // If no shader has been loaded yet, nothing to render.
        if (m_shaderBytecode.empty() && !m_shaderLoaded)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (m_shaderDirty && !m_shaderBytecode.empty())
        {
            if (m_shaderGuid == GUID{})
                CoCreateGuid(&m_shaderGuid);

            HRESULT hr = m_effectContext->LoadComputeShader(
                m_shaderGuid,
                m_shaderBytecode.data(),
                static_cast<UINT32>(m_shaderBytecode.size()));

            if (SUCCEEDED(hr))
                hr = m_computeInfo->SetComputeShader(m_shaderGuid);

            if (FAILED(hr))
                return hr;

            m_shaderDirty = false;
            m_shaderLoaded = true;
        }

        // The cbuffer is not uploaded here: D2D latches it at this point,
        // which would freeze the per-tile _TileOffset CalculateThreadgroups
        // writes.

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1ComputeTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::SetComputeInfo(ID2D1ComputeInfo* computeInfo)
    {
        m_computeInfo.copy_from(computeInfo);

        // Set output buffer to FP16 for HDR/WCG support.
        computeInfo->SetOutputBuffer(D2D1_BUFFER_PRECISION_16BPC_FLOAT, D2D1_CHANNEL_DEPTH_4);

        if (!m_shaderBytecode.empty())
            m_shaderDirty = true;

        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::CalculateThreadgroups(
        const D2D1_RECT_L* outputRect,
        UINT32* dimensionX,
        UINT32* dimensionY,
        UINT32* dimensionZ)
    {
        UINT32 width  = static_cast<UINT32>(outputRect->right - outputRect->left);
        UINT32 height = static_cast<UINT32>(outputRect->bottom - outputRect->top);
        if (width == 0) width = 1;
        if (height == 0) height = 1;

        // Upload the constant buffer here (NOT in PrepareForRender) so that
        // per-tile system constants are correct for each tile dispatch.
        // D2D calls CalculateThreadgroups once per tile, after
        // MapInputRectsToOutputRect has set the tile region.
        if (m_computeInfo && !m_constantBuffer.empty())
        {
            // Inject system constant into reserved cbuffer prefix:
            //   offset  0: int2 _TileOffset  (tile origin in full image)
            // User properties start at offset 8+.
            // NOTE: _ImageSize is NOT injected — shaders must use
            // Source.GetDimensions() which returns the full source size.
            if (m_constantBuffer.size() >= 8)
            {
                int32_t offX = outputRect->left;
                int32_t offY = outputRect->top;
                memcpy(m_constantBuffer.data() + 0, &offX, sizeof(int32_t));
                memcpy(m_constantBuffer.data() + 4, &offY, sizeof(int32_t));
            }

            m_computeInfo->SetComputeShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));
            m_cbDirty = false;
        }

        *dimensionX = (width  + m_threadGroupX - 1) / m_threadGroupX;
        *dimensionY = (height + m_threadGroupY - 1) / m_threadGroupY;
        *dimensionZ = 1;

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            *outputRect = inputRects[0];
        }
        else
        {
            m_inputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
            *outputRect = m_inputRect;
        }

        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // If MapInputRectsToOutputRect hasn't been called yet (no valid input rect),
        // fall back to the requested output rect so D2D gets reasonable bounds.
        D2D1_RECT_L rect = m_inputRect;
        if (rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0 && outputRect)
            rect = *outputRect;

        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = rect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L /*invalidInputRect*/,
        D2D1_RECT_L* invalidOutputRect) const
    {
        // Return the stored input rect, or empty if not yet initialized.
        *invalidOutputRect = m_inputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::ForceUploadConstantBuffer()
    {
        if (!m_computeInfo || m_constantBuffer.empty())
            return E_FAIL;
        m_cbDirty = false;
        return m_computeInfo->SetComputeShaderConstantBuffer(
            m_constantBuffer.data(),
            static_cast<UINT32>(m_constantBuffer.size()));
    }

    void CustomComputeShaderEffect::SetThreadGroupSize(UINT32 x, UINT32 y)
    {
        m_threadGroupX = (std::max)(x, 1u);
        m_threadGroupY = (std::max)(y, 1u);
    }
}
