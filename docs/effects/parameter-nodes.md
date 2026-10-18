# Parameter Nodes

Parameter nodes are data-only graph elements (no HLSL shader) that expose a single value for binding to downstream effect properties.

- **Five built-in parameter types**: Float, Integer, Toggle, Gamut, Clock. The formula-driven [Numeric Expression](#numeric-expression-node-exprtk) is parameter-like (also data-only, also produces a `Result` analysis field) but takes one or more `float` inputs.
- **Teal color** on the node graph canvas.
- **Inline slider**: rendered directly on the D2D canvas for quick adjustment (Float / Integer / Toggle / Gamut).
- **No preview switch**: clicking a parameter node does not change the preview target.
- **Evaluator-populated**: the graph evaluator populates analysis output fields directly from node properties (no shader dispatch).


---

Back to [docs/](../README.md) • [Repo root](../../README.md)