#include "pch_engine.h"
#include "CustomPixelShaderEffect.h"

namespace ShaderLab::Effects
{
    namespace
    {
        // Output extent for an edge no input bounds (Flood, Tile, Border,
        // Turbulence). Bounded inputs keep their own size.
        constexpr LONG cUnboundedOutputExtent = 4096;

        // D2D reports an infinite edge as a value near the LONG limits.
        constexpr LONG cUnboundedEdge = 1L << 24;

        bool IsUnboundedEdge(LONG edge)
        {
            return edge <= -cUnboundedEdge || edge >= cUnboundedEdge;
        }
    }

    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        return AttachToGraph(transformGraph);
    }

    IFACEMETHODIMP CustomPixelShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_drawInfo)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (HRESULT hr = ForceLoadShader(); FAILED(hr))
            return hr;

        // If the constant buffer data changed, push it to the GPU.
        if (m_cbDirty && !m_constantBuffer.empty())
        {
            HRESULT hr = m_drawInfo->SetPixelShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));

            if (FAILED(hr))
                return hr;

            m_cbDirty = false;
        }

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1DrawTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);

        if (!m_shaderBytecode.empty())
            m_shaderDirty = true;

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        // Source effects with a fixed output size always use it,
        // regardless of input rect (which may be a dummy bitmap).
        if (m_fixedOutputWidth > 0 && m_fixedOutputHeight > 0)
        {
            *outputRect = D2D1_RECT_L{ 0, 0,
                static_cast<LONG>(m_fixedOutputWidth),
                static_cast<LONG>(m_fixedOutputHeight) };
            m_inputRect = *outputRect;
            m_lastOutputRect = *outputRect;
        }
        else if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            // Remember every input's own extent. Non-image pins are answered
            // with it in MapOutputRectToInputRects -- see there.
            m_allInputRects.assign(inputRects, inputRects + inputRectCount);

            // Union of the IMAGE input rects only. Pins past that point carry
            // analysis textures (1 x N texels) for gpu-bindable parameters;
            // they say nothing about where this effect draws, and including
            // them collapses the output rect so the node renders nothing.
            const UINT32 imageInputs = (std::min)(inputRectCount, ImageInputCount());
            *outputRect = inputRects[0];
            for (UINT32 i = 1; i < imageInputs; ++i)
            {
                outputRect->left   = (std::min)(outputRect->left,   inputRects[i].left);
                outputRect->top    = (std::min)(outputRect->top,    inputRects[i].top);
                outputRect->right  = (std::max)(outputRect->right,  inputRects[i].right);
                outputRect->bottom = (std::max)(outputRect->bottom, inputRects[i].bottom);
            }

            // TEXCOORD is normalized over this rect, so an infinite one maps
            // every pixel to the same texel. Unbounded edges are pinned to
            // the origin and to the widest bounded input or the default.
            LONG boundedRight = 0;
            LONG boundedBottom = 0;
            for (UINT32 i = 0; i < imageInputs; ++i)
            {
                if (!IsUnboundedEdge(inputRects[i].right))
                    boundedRight = (std::max)(boundedRight, inputRects[i].right);
                if (!IsUnboundedEdge(inputRects[i].bottom))
                    boundedBottom = (std::max)(boundedBottom, inputRects[i].bottom);
            }
            outputRect->left = (std::max)(outputRect->left, 0L);
            outputRect->top  = (std::max)(outputRect->top,  0L);
            if (IsUnboundedEdge(outputRect->right))
                outputRect->right = (std::max)(cUnboundedOutputExtent, boundedRight);
            if (IsUnboundedEdge(outputRect->bottom))
                outputRect->bottom = (std::max)(cUnboundedOutputExtent, boundedBottom);

            m_lastOutputRect = *outputRect;
        }
        else
        {
            *outputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
            m_lastOutputRect = *outputRect;
        }

        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // Honour the queried sub-rect for IMAGE inputs.
        //
        // Demanding the whole input for every query is only free while D2D
        // renders the output in one pass. Past ~2048 px WIDE it tiles, asks
        // for each tile in turn, and a whole-input answer makes it re-render
        // the entire upstream chain once per tile. Measured cliff: 2048x1280
        // 1.10 ms, 2100x1280 56.05 ms -- 1.03x the pixels, 51x the time.
        //
        // TRIVIAL_SAMPLING is declared, so output pixel N depends only on
        // input pixel N and the 1:1 answer is the correct one.
        //
        // Analysis pins (>= ImageInputCount) are 1 x N parameter textures and
        // say nothing about geometry; they keep the old answer.
        if (!m_subRectInputDemand)
            return MapOutputRectToInputRectsWholeInput(
                outputRect, inputRects, inputRectCount);

        const UINT32 imageInputs = (std::min)(inputRectCount, ImageInputCount());
        for (UINT32 i = 0; i < imageInputs; ++i)
            inputRects[i] = *outputRect;
        // Lookup tables and analysis textures are read at COMPUTED coordinates,
        // so every tile needs all of them -- demand each one's own full extent.
        // Not the output rect: asking a 256x128 table for 2880x1800 invites D2D
        // to build an intermediate that size around it on every frame.
        for (UINT32 i = imageInputs; i < inputRectCount; ++i)
            inputRects[i] = (i < m_allInputRects.size()) ? m_allInputRects[i]
                                                         : m_lastOutputRect;
        return S_OK;
    }

    // Whole-input demand, the subRectInputDemand = false kill switch (see
    // EvaluatorOptions::subRectInputDemand for what it costs).
    IFACEMETHODIMP CustomPixelShaderEffect::MapOutputRectToInputRectsWholeInput(
        const D2D1_RECT_L* /*outputRect*/,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // Demand the full output rect from every input, ignoring the queried
        // sub-rect, so each intermediate matches the whole effect output.
        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = m_lastOutputRect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        // Any change in an input invalidates the same region of the output.
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::ForceLoadShader()
    {
        if (!m_shaderDirty || m_shaderBytecode.empty())
            return S_OK;
        // Before D2D has initialised the effect, its first PrepareForRender loads it.
        if (!m_effectContext || !m_drawInfo)
            return S_OK;
        if (m_shaderGuid == GUID{})
            CoCreateGuid(&m_shaderGuid);

        HRESULT hr = m_effectContext->LoadPixelShader(
            m_shaderGuid,
            m_shaderBytecode.data(),
            static_cast<UINT32>(m_shaderBytecode.size()));
        if (SUCCEEDED(hr))
            hr = m_drawInfo->SetPixelShader(m_shaderGuid, D2D1_PIXEL_OPTIONS_TRIVIAL_SAMPLING);
        if (FAILED(hr))
            return hr;
        m_shaderDirty = false;
        return S_OK;
    }

    HRESULT CustomPixelShaderEffect::ForceUploadConstantBuffer()
    {
        if (!m_drawInfo || m_constantBuffer.empty())
            return E_FAIL;
        m_cbDirty = false;
        return m_drawInfo->SetPixelShaderConstantBuffer(
            m_constantBuffer.data(),
            static_cast<UINT32>(m_constantBuffer.size()));
    }
}
