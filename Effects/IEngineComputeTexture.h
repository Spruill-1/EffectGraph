#pragma once

// IEngineComputeTexture -- the third output lane of a compute-analysis effect.
//
// A compute analysis node can publish its Result[] in three shapes, and which
// ones it materialises depends on who is actually consuming it:
//
//   1. StructuredBuffer SRV  (IEngineComputeOutput::GetAnalysisSrv)
//        For a COMPUTE consumer. Bound straight to a t-slot; the cheapest
//        lane.
//   2. CPU values            (EffectNode::analysisOutput.fields)
//        For anything CPU-side: a Numeric Expression, the properties panel,
//        an MCP read. Costs a Map() that waits on the GPU, which destroys
//        CPU/GPU overlap rather than merely copying a few floats.
//   3. A 1-row RGBA32F TEXTURE  (this interface)
//        For a PIXEL-shader consumer. Direct2D will not let a custom pixel
//        shader bind an arbitrary SRV -- it maps a transform's INPUTS to
//        Texture2D at t0, t1, ... and nothing else -- and the obvious way
//        round, ID2D1EffectContext::CreateResourceTexture, takes `const
//        BYTE*` CPU memory, so it would reintroduce exactly the readback
//        lane 3 exists to avoid. Publishing the values as a tiny image and
//        wiring it in as an ordinary effect input keeps them GPU-resident.
//
// Lane 3 is OPT-IN: the texture and its copy pass are not created until a
// consumer calls RequestAnalysisTexture(). A graph with no pixel-shader
// consumer never pays for it.
//
// Layout: texel i of row 0 holds Result[i], so field lookup is the same
// prefix-sum over `analysisFields` that lane 1 uses. As with
// IEngineComputeOutput, the field->index mapping is deliberately NOT part of
// this interface: it lives on EffectNode::analysisOutput.fields, and the
// evaluator has the node in hand.


struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace ShaderLab::Effects
{
    // {5E1C77A4-2D3B-4F08-9C61-7A0E4B26D9F3}
    constexpr GUID IID_IEngineComputeTexture = {
        0x5e1c77a4, 0x2d3b, 0x4f08,
        { 0x9c, 0x61, 0x7a, 0x0e, 0x4b, 0x26, 0xd9, 0xf3 }
    };

    struct __declspec(uuid("5E1C77A4-2D3B-4F08-9C61-7A0E4B26D9F3"))
    IEngineComputeTexture : IUnknown
    {
        // Ask for lane 3. Until this is called the texture does not exist and
        // no copy pass runs, which is the whole point: a graph that has no
        // pixel-shader consumer should not pay to materialise one.
        // Idempotent, and valid before the runner is initialised. Returns S_OK;
        // the texture is filled on the next dispatch.
        virtual HRESULT STDMETHODCALLTYPE RequestAnalysisTexture() = 0;

        // The 1 x N RGBA32F texture holding Result[]. Wrapped by the caller
        // as a D2D bitmap and set as an effect input. Returns
        // E_NOT_VALID_STATE if RequestAnalysisTexture() has not been called
        // or no dispatch has filled it yet.
        virtual HRESULT STDMETHODCALLTYPE GetAnalysisTexture(
            ID3D11Texture2D** out) = 0;

        // SRV over the same texture, for a consumer that can bind one
        // directly rather than going through D2D.
        virtual HRESULT STDMETHODCALLTYPE GetAnalysisTextureSrv(
            ID3D11ShaderResourceView** out) = 0;
    };
}
