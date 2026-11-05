# Compute Shader Analysis Pipeline

Custom compute shaders can act as analysis effects, producing typed output fields that are read back to the CPU and can drive downstream effect properties via data bindings.

## Analysis Output Types

| Type | Pixels | Packing |
|------|--------|---------|
| `float` | 1 | `.x` used |
| `float2` | 1 | `.xy` used |
| `float3` | 1 | `.xyz` used |
| `float4` | 1 | all 4 components |
| `floatarray` | ceil(N/4) | 4 floats packed per pixel |
| `float2array` | N | `.xy` per pixel |
| `float3array` | N | `.xyz` per pixel |
| `float4array` | N | all 4 per pixel |

## D2D Compute Shader Conventions

D2D evaluates compute effects in **tiles**. Key conventions:

- **`_TileOffset`** (int2): Auto-injected at cbuffer offset 0 in `CalculateThreadgroups`. Gives the tile's origin in the full image.
- **`Source.GetDimensions()`**: Returns the full source image size (not tile size).
- **`SampleLevel()`**: Must use normalized UVs via `SampleLevel()`. `Load()` is not available in D2D compute shaders.
- **`Output.GetDimensions()`**: Returns the tile size, not the full image.
- **Constant buffer upload**: Done in `CalculateThreadgroups` (not `PrepareForRender`) for correct per-tile values.

## Statistics reductions

The statistics shaders (Channel, Luminance and Chromaticity Statistics) share
the multi-group reduction scaffolding from `shaderlab_params.hlsli`:
`SHADERLAB_PARTIAL_WORD(group, i)` and `SHADERLAB_HISTOGRAM_WORD(group, bin, bins)`
lay out the scratch buffer, `ShaderLabTreeReduce(tid, threads, combine)` folds
groupshared accumulators through the shader's own `combine(dst, src)`,
`ShaderLabStoreHistogram` / `ShaderLabMergeHistograms` move histograms through
scratch, and `ShaderLabPercentileBin` walks the merged histogram. Each shader
keeps only its per-pixel accumulate, its `Combine` and its `Result` packing.
Each macro is a braced block, so its loop variables cannot collide with the
caller's (fxc scopes a `for` initialiser to the enclosing block).

Since Channel Statistics 3, Luminance Statistics 7 and Chromaticity Statistics 3
the three reduce over **cells** (`SHADERLAB_REDUCE_CELLS`, decision #256): input 0
is cut into an 8 x 8 grid (`ShaderLabCellRect(cell, width, height)`: column `cx`
spans `cx * W / 8` to `(cx + 1) * W / 8`, rows alike), each group reduces one cell
(`ShaderLabReduceCell(Gid.x)` names it from the `_SLReduceCells` cbuffer at `b2`)
into that cell's partial slot, and the last group to finish folds all 64 slots
(`SlotCount`) in slot order. The slots live above the cleared head of the
persistent scratch buffer, so a cell nobody recomputes keeps its partial. When
the evaluator knows the input changed only in rects (`EvaluatorOptions::
incrementalReductions`, on), `D3D11ComputeRunner` dispatches only the cells the
rects touch, and the fold reads the other cells' kept partials. Every field is
bit-identical to a dispatch that recomputed all 64, because each thread walks
its cell in the same order (32 pixels at a time in both axes) and the fold
order is fixed. `ShaderLabMergeHistogramSlots` merges the cells' histograms.
Gamut Coverage and CIE Histogram keep their reductions.

## Shader Pattern
```hlsl
cbuffer Constants : register(b0) {
    int2 _TileOffset;  // Auto-injected per tile
    // User parameters here...
};
Texture2D Source : register(t0);
RWTexture2D<float4> Output : register(u0);
SamplerState Sampler0 : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint srcW, srcH;
    Source.GetDimensions(srcW, srcH);
    uint2 globalPos = DTid.xy + uint2(_TileOffset);
    if (globalPos.x >= srcW || globalPos.y >= srcH) return;
    
    float2 uv = (float2(globalPos) + 0.5) / float2(srcW, srcH);
    float4 color = Source.SampleLevel(Sampler0, uv, 0);
    Output[DTid.xy] = color;
}
```


---

Back to [docs/](../README.md) • [Repo root](../../README.md)