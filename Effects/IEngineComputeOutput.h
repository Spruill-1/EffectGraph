#pragma once

// IEngineComputeOutput — engine-internal COM interface exposed by
// effects whose analysis output is GPU-resident in a structured buffer.
//
// When a compute analysis effect's output field feeds a downstream effect's
// parameter through a property binding, the value can travel GPU to GPU
// instead of round-tripping through Map() (lane 1 in
// docs/architecture/d2d-d3d11-hybrid-compute.md). The evaluator detects this routing by QI'ing the
// upstream effect for this interface; effects that implement it expose
// the SRV of their structured-buffer output, and the evaluator binds
// it directly to the downstream effect's t-slot for the appropriate
// SHADERLAB_GPU_BUFFER prologue.
//
// Field layout (which Result[N] slot maps to which named field) is
// **NOT** part of this interface. It already lives in the graph data
// model on `EffectNode::analysisOutput.fields`. The evaluator has the
// EffectNode in hand at QI time, so duplicating the metadata across
// the COM boundary would only invite drift. This interface is narrow
// on purpose: just the runtime resource handles, in stable POD shape.
//
// Implementations: CustomComputeBridgeEffect (the D3D11-compute path) and
// its D3D11ComputeRunner. Pixel-shader effects and the D2D-tiled compute
// path do not implement it -- neither produces a structured-buffer SRV. A
// pixel-shader consumer reads the values through IEngineComputeTexture.


struct ID3D11ShaderResourceView;

namespace ShaderLab::Effects
{
    // {831B9291-CCAB-40A2-B0BA-E847F5B9FA6C}
    constexpr GUID IID_IEngineComputeOutput = {
        0x831b9291, 0xccab, 0x40a2,
        { 0xb0, 0xba, 0xe8, 0x47, 0xf5, 0xb9, 0xfa, 0x6c }
    };

    struct __declspec(uuid("831B9291-CCAB-40A2-B0BA-E847F5B9FA6C"))
    IEngineComputeOutput : IUnknown
    {
        // Returns the SRV of the upstream effect's structured-buffer
        // output. Caller AddRef's via the COM out-param convention; the
        // SRV's lifetime is owned by the upstream effect and remains
        // valid until the upstream effect is invalidated or destroyed.
        // Returns S_OK on success or E_NOT_VALID_STATE if the upstream
        // effect has not yet been evaluated this session.
        virtual HRESULT STDMETHODCALLTYPE GetAnalysisSrv(
            ID3D11ShaderResourceView** out) = 0;

        // Frame counter that the SRV's contents were last written on.
        // The evaluator uses this to detect stale data: a downstream
        // QI hit on a frame where the upstream wasn't re-evaluated
        // means the consumer would read last frame's value, which may
        // still be acceptable depending on the consumer's tolerance.
        // Returns 0 if the upstream effect has never been evaluated.
        virtual UINT64 STDMETHODCALLTYPE GetLastEvaluatedFrame() = 0;
    };
}
