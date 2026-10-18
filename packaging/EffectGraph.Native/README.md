# EffectGraph.Native

EffectGraph is ShaderLab's effect library and graph evaluator for any program
that has a D3D11 device: HDR tone mapping, gamut mapping, statistics and
analysis effects, evaluated as a graph on your device, in linear scRGB
(1.0 = 80 nits).

This package is the native library: `EffectGraph.dll` for x64 and ARM64, its
import libraries, `EffectGraph.h`, a header and an `.effectblob` per built-in
effect, and `EffectGraphCompile` for building your own effects. For C#, use the
`EffectGraph` package, which depends on this one.

## C++

Reference the package from an x64 or ARM64 project; the include path, the
import library and the copy of `EffectGraph.dll` beside your output are set
up for you. Include the header of each built-in you use; it registers itself.

```cpp
#include "EffectGraph.h"
#include "luminance_statistics.effect.h"

winrt::com_ptr<IEffectGraphContext> context;
EffectGraphCreateContext(d3dDevice, nullptr, IID_PPV_ARGS(context.put()));
winrt::com_ptr<IEffectGraphNode> input, stats;
context->CreateTextureInput(hdrTexture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put());
context->CreateEffectById(CLSID_EffectGraphLuminanceStatistics, stats.put());
stats->SetInput(0, input.get());
winrt::com_ptr<IEffectGraphValues> values;
stats->GetValues(values.put());
float peak = 0;
values->GetFloat(L"Max", &peak);
```

Your own effects, saved from ShaderLab's Effect Designer, are compiled at your
build: add `<EffectGraphEffect Include="MyEffect.json" />` to the project and
include `MyEffect.effect.h`.

## Other languages

`effects/` holds every built-in as an `.effectblob` for
`EffectGraphRegisterEffect`, and `runtimes/win-x64/native` and
`runtimes/win-arm64/native` hold the DLL.

Documentation: <https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/effectgraph-api.md>
