# Option Variants (compile-time specialisation)

An **option parameter** — one with a fixed list of choices (`enumLabels`), such
as a target gamut or a mode — can be compiled into a **separate shader per
option**, with the value a compile-time constant. The compiler then folds away
everything the other options need: a target's colour matrices become literals,
an identity path disappears, a disabled output stage is dropped.

ShaderLab compiles **every variant as soon as the effect is placed** (or its
graph is opened), so changing an option later only switches to bytecode that
already exists. Nothing recompiles while you tinker.

## Opting in

In the [Effect Designer](https://github.com/Spruill-1/ShaderLab/blob/main/docs/effects/effect-designer.md), give an `enum` parameter its
labels and tick **Variant per option**. The generated HLSL uses the macros
below. In a saved graph or a user effect, the flag is `"specialize": true` on
the parameter.

By hand, write two pieces, as with `SHADERLAB_PARAM`:

```hlsl
#include "shaderlab_params.hlsli"

cbuffer Constants : register(b0)
{
    SHADERLAB_OPTION(uint, TargetGamut)   // instead of `uint TargetGamut;`
    float2 RedPrimary;                    // ordinary members are unchanged
    ...
};
SHADERLAB_OPTION_VALUE(uint, TargetGamut) // file scope, after the cbuffer
```

and use `TargetGamut` exactly as before. The include is the optional
`shaderlab_params.hlsli` [engine header](https://github.com/Spruill-1/ShaderLab/blob/main/docs/effects/effect-designer.md#engine-headers);
a shader that uses these macros needs it. The host always defines
`_SLOPT_<name>_MODE` (0 = generic, 1 = fixed) and `_SLOPT_<name>` (the option
index), so the source compiles identically in every variant:

| Build | `SHADERLAB_OPTION(uint, X)` in the cbuffer | `SHADERLAB_OPTION_VALUE(uint, X)` |
|---|---|---|
| generic | `uint X;` | *(nothing)* |
| variant for option *v* | `uint _SLOptSlot_X;` (placeholder: every later member keeps its offset) | `static const uint X = v;` |

Rules:

- **Only CPU option parameters qualify**: `enumLabels` non-empty, not
  `gpuBindable` (a value that arrives on the GPU cannot choose a variant on the
  CPU). At most **8** specialised parameters per effect, and at most **64**
  combinations are prebuilt; past that, only the combinations actually shown
  are compiled, on demand.
- **"Custom" is just another option.** Parameters that only one option reads —
  a Custom gamut's primaries and white point — stay ordinary cbuffer members.
  In the Custom variant they are live (bind them to Working Space as usual,
  no recompiles); in the others they fold away, and the host skips cbuffer
  names a variant does not declare.
- **A value that is not an option** — out of range, or fractional from a
  binding — selects the **generic build**, which reads the option from the
  cbuffer and is always correct. The cbuffer truncates, so a value at most
  1e-4 *above* an option picks that option's variant, and one just *below* it
  picks the generic build, which reads the option before it.

## What the host does

- **Up front.** When a node's shader is first requested, the evaluator queues
  every option combination on the bytecode cache's worker threads (half the
  cores, 2–6), the combination the node shows now first. Variants already in
  the on-disk cache (`%LOCALAPPDATA%\ShaderLab\bytecode`) are loaded, not
  recompiled, so this happens once per shader, not once per launch.
- **While they compile** the node renders with its generic build. The status
  bar reads *Precompiling N shader variants…*; the modal *Recompiling shaders*
  dialog is only for a node that has no output at all. The fallback order is
  the wanted variant, then the same GPU routing without the options, then the
  options without the GPU routing, then the generic baseline; only the wanted
  one is compiled urgently, so in practice the fallback is the baseline. The
  baseline reads GPU-bindable parameters from the cbuffer, so while it runs,
  the analysis values they are bound to are read back to the CPU every frame
  even with *Keep analysis on the GPU* on (decision #114).
- **On an option change** the evaluator switches variant: a memo hit, a new
  shader GUID for Direct2D, derived from the variant's bytecode (Direct2D
  ignores a reload under a GUID it already holds; decision #108), and the
  cbuffer is re-packed by reflection. The new shader is loaded in the same
  evaluation as its cbuffer, not at Direct2D's next `PrepareForRender`, which
  never comes for a node that is dirty every frame (decision #114).
- **Headless** compiles only the variant it renders (synchronously, cached on
  disk), so a script never waits on compiles it did not need.

The cache key carries the option values (`BytecodeCompileKey::optionKey`,
8 bits per parameter: 0 generic, *v*+1 option *v*) alongside the GPU-binding
modes, and the option names join the parameter signature hash, so variants of
different definitions can never collide. `Effects/ShaderVariants.h` owns all of
it: `MakeVariantRequest` builds every request (so a key the evaluator asks for and
one queued elsewhere always match), `BuildVariantMacros` spells the macros, and
`PrecompileBindingShapes` warms the GPU-binding shapes of an effect with no
options; `BytecodeCache` only stores and compiles. The evaluator side is
`Rendering/Evaluator/VariantSelector.cpp` (`Select`, `QueueOptionVariants`,
`Poll`, `CompileBaseline`), called from `ApplyCustomEffect`
(`CustomEffectNode.cpp`) and `DispatchViaBridge` (`ComputeScheduler.cpp`).

## Why not a constants table?

The first attempt at removing per-pixel setup work put the frame constants in a
small table computed by a helper node and read by the shader. It was **slower**
(+2.4 ms on a 4K frame): values computed only from the cbuffer are uniform, and
the GPU already evaluates them roughly once per wave, whereas a texture load is
per-lane data that costs registers and occupancy. The gain measured by
hard-coding the values came from *constant folding*, which only compile-time
specialisation provides. See [decision #106](../history/decision-log.md).
