# User Effects

A user effect is a ShaderLab effect that lives outside the build: registered at
startup from a file, it appears in the **Add Effect** menu, can be created by name
with `graph_add_node`, and gets the same "Update Effect" prompt as a built-in when a
graph holds an older copy of it.

## The file format is a saved graph

There is no separate format. A saved graph already embeds each shader node's full
definition (HLSL, parameters, pins, lookup-input count, `shaderLabEffectId`,
`shaderLabEffectVersion`), which is why a graph keeps rendering a node whose effect
is not registered anywhere. Registering that definition is all a user effect adds.

So **exporting an effect is saving a graph that contains it.** Every shader node in
the file (any node with a `customEffect`) becomes one effect:

| Descriptor field | Comes from |
|---|---|
| name | the node's name |
| effect id / version | `customEffect.shaderLabEffectId` / `shaderLabEffectVersion` (id falls back to the name; version to 1) |
| HLSL, inputs, lookup inputs, parameters, analysis fields, thread groups | the node's `customEffect` |
| node kind, dispatch shape, output extent, injected values | the node's `customEffect` contracts, derived on load when the file predates them ([Node contracts](../architecture/effect-graph-model.md#node-contracts)) |
| parameter defaults | each parameter's declared `default` (the node's current values are ignored) |
| hidden cbuffer values | node properties that are not declared parameters |
| category | optional top-level `"category"` string in the file, default `User` |

Source nodes, D2D built-in nodes and output nodes in the file are ignored, so a file
can also carry a small example graph around the effect.

The HLSL may `#include "shaderlab_colormath.hlsli"` and
`#include "shaderlab_params.hlsli"`, the two
[engine headers](https://github.com/Spruill-1/ShaderLab/blob/main/docs/effects/effect-designer.md#engine-headers); both are optional. An effect
that includes the color-math header uses the library of the ShaderLab build that
loads it. An effect that must keep older math pastes the functions it needs in
place of the include. Saved graphs are never rewritten from one form to the other.

## Where they load from

| Host | Directory |
|---|---|
| GUI | `%LOCALAPPDATA%\ShaderLab\effects\*.json`, at startup |
| Headless | only what `--effects-dir DIR` names (repeatable) |

Headless never reads the GUI's folder on its own, so a headless result cannot depend
on what happens to be installed on the machine running it.

Loading happens once, before any graph is evaluated: registering appends to the
effect registry, which the render worker and the MCP routes read without locking.
To pick up a changed file, restart the app.

## What happens when something is wrong

Nothing is skipped silently. Each problem becomes one line in the load report
(`ShaderLabEffects::UserEffectReport()`):

- the file is not UTF-8 JSON, or not a ShaderLab graph;
- it contains no shader nodes;
- a node's HLSL does not compile (checked exactly as the evaluator compiles it,
  with the `_SLPARAM_<name>_GPU=0` macros of its cbuffer variant);
- a node's id or name is already registered, and the conflict policy says keep the
  existing effect.

The report surfaces in each host:

- **GUI:** a disabled "*N* user effects failed to load" item at the bottom of the
  ShaderLab section of **Add Effect**, whose tooltip lists the reasons.
- **Headless:** one `[ShaderLab] user effect not loaded: …` line per problem on stderr.
- **MCP:** `list_effects` returns `user.directories`, `user.loaded` and
  `user.errors` next to the catalog.

A missing directory is not an error; most machines have none.

## Conflicts

A user effect conflicts with an existing one when either its **id** (which keys
saved-graph upgrades) or its **name** (which keys the menu and `graph_add_node`)
is already registered, by a built-in or by an earlier file. Files load in sorted
filename order, so the outcome never depends on directory enumeration order.
The rule (`ResolveUserEffectConflict`):

- **A built-in is never replaced.** Replacing one would silently change what every
  saved graph using that id upgrades to, and would drop built-in-only hooks.
- **Between user effects, the higher `effectVersion` wins.** Dropping a newer file
  next to an older one upgrades the effect. If the older file loads first it is
  superseded in place; if it loads second it is reported as not loaded.
- **A tie keeps the first file loaded**, and the second is reported.

## Relationship to the built-in library

- `LibraryVersion()` (the "effects lib vN" in the title bar) counts built-ins only,
  so it keeps identifying the build.
- The catalog-count and pixel-shader-signature tests pin built-ins only.
- A user effect gets no built-in-only hooks (`deriveConstants`, `deriveImageOutputSize`, clock behaviour).
