# Property Bindings (Data Pins)

Analysis output fields can be visually connected to downstream effect properties using **data pins** on the node graph canvas.

```mermaid
graph LR
    subgraph Compute["Analysis Compute"]
        CO1["◆ MaxLuminance (float)"]
        CO2["◆ GamutBounds (float4)"]
    end
    subgraph Blur["Gaussian Blur"]
        BI1["◆ StandardDeviation (float)"]
    end
    CO1 -->|"data binding"| BI1
```

## Visual Data Pins

- **Image pins**: White circles on node edges (existing D2D image connections)
- **Data pins**: Orange diamonds below image pins
  - Output data pins (right side): analysis fields from compute nodes
  - Input data pins (left side): bindable float/float2/float3/float4 properties
- **Data edges**: Orange bezier curves connecting data pins
- **Type labels**: Each pin shows its type, e.g., `MaxLuminance (float)`

## Binding Rules

| Source → Dest | Behavior |
|---|---|
| float → float | Direct |
| float → float2/3/4 | Replicate (x,x,x,0) |
| float4 → float | Component picker (.x/.y/.z/.w) |
| float4 → float4 | Direct |
| array → array | Direct |
| array ↔ scalar | Rejected |

## Evaluation

- Bindings participate in **topological sort** (source must evaluate before destination)
- **Cycle detection** covers both image edges and binding edges
- Bound values resolved **every frame** (bypass dirty logic — upstream analysis may change)
- **Authored properties never mutated** — bindings build an effective properties map at evaluation time
- **Write-back for display**: `GraphEvaluator::ResolveAndWriteBackBindings` copies each moved value into `properties` (what the UI shows and a save writes), except a non-finite one, which keeps the previous value and sets `runtimeError`; every node kind goes through it
- **One traversal**: `PropertyBinding::ForEachSourceNode` / `AnySourceNode` / `ReadsFrom` and `ForEachBindingSource` / `AnyBindingSource` (`Graph/EffectNode.h`) visit the nodes a binding reads, whole-array or per-component, for the sort, dirty propagation, removal and the canvas
- **Removing one source**: `GraphModel::UnbindPropertySource` drops the components that read a given source field and the whole binding when none is left (Alt+click on a data edge)


---

Back to [docs/](../README.md) • [Repo root](../../README.md)