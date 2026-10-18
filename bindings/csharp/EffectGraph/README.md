# EffectGraph for .NET

C# bindings for EffectGraph, ShaderLab's effect library and graph evaluator for
D3D11 and Direct2D programs: HDR tone mapping, gamut mapping, statistics and
analysis effects, in linear scRGB (1.0 = 80 nits). Windows, x64 and ARM64; the
native `EffectGraph.dll` comes with the `EffectGraph.Native` dependency.

Direct3D and Direct2D objects cross as `IntPtr` (`ID3D11Device*`,
`ID3D11Texture2D*`, `ID2D1Image*`), so the bindings work with any D3D wrapper
or none.

```csharp
using EffectGraph;

BuiltInEffects.Register("Luminance Statistics");      // built-ins are embedded; register what you use
var context = EffectGraphLibrary.CreateContext(d3d11Device);   // BGRA-capable ID3D11Device*
var input = context.CreateTextureInput(hdrTexture, InputEncoding.ScRgbLinear);
var stats = context.CreateEffect("Luminance Statistics");
stats.SetInput(0, input);
float peak = stats.GetValues().GetFloat("Max");        // nits

// The same values on the GPU, for your own shaders.
var buffer = stats.GetValuesBufferOrNull();
buffer!.GetView(out IntPtr shaderResourceView);
```

Failures arrive as `COMException`, with `HResult` one of `HResults` (for
example `HResults.EffectNotIncluded` for a built-in you did not register).

Documentation: <https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/effectgraph-api.md>
