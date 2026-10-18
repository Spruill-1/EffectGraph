# Numeric Expression Node (ExprTk)

A single, expression-driven math node replaces the legacy fixed-op math primitives. The engine evaluates it through a hook, `Rendering/MathExpression.{h,cpp}`; a host registers its expression language with `Rendering::SetMathExpressionEvaluator`. EffectGraph has none of its own: without a registration every expression fails with an error saying so (decision #224). ShaderLab registers ExprTk; the notes below on ExprTk describe that evaluator.

- **Dynamic inputs**: starts with one input `A`. Use the **➕ Add Input** button in the Properties panel to add `B`, `C`, … up to `Z` (26-input cap). Each row has an **✕** button to remove just that input. Both are engine edits in `Graph/GraphEdits` (`AddExpressionInput`, `RemoveExpressionInput`, `CanRemoveExpressionInput`), so removing an input also drops any binding to it and the last input cannot go.
- **Expression**: a `TextBox` at the top of the Properties panel holds the formula. Examples:
  - `A + B * C`
  - `max(A, B, C)`
  - `if (A > B, A, B)`
  - `sin(A) * 0.5 + 0.5`
  - `pow(clamp(A, 0, 1), 2.2)`
- **Canvas display**: the formula is rendered under the node title as `= <expression>` so it's readable at a glance in the graph.
- **Output**: a single `float` analysis field named `Result`, available on the orange data pin and bindable to any downstream scalar property.
- **Errors**: parse / evaluation errors surface in the per-node log pane (`node_logs` MCP tool). The node short-circuits to `0.0` until the expression compiles cleanly.
- **Persistence**: the expression and the live input list both round-trip through graph JSON.

**ExprTk feature flags.** `Support/ExprTkEvaluator.cpp` defines
`exprtk_disable_string_capabilities`, `exprtk_disable_rtl_io`, `exprtk_disable_rtl_io_file`,
`exprtk_disable_rtl_vecops`, `exprtk_disable_enhanced_features`, and
`exprtk_disable_caseinsensitivity` *before* `#include`-ing `exprtk.hpp`. This trims the
library to its math-only core; without these flags the MSVC Release optimizer was
producing a 0xC0000005 in the regex / IO subsystems on first evaluation. Only finite
scalar expressions are supported — no strings, files, or vector return values.

**Loops are refused.** Expressions come from saved graphs and MCP and are evaluated on
the render thread, where `while (1) {}` would hang it. The parser is built with the
`for`, `while` and `repeat` control structures disabled, so an expression using one
fails to compile and the node reports the parse error like any other (tests
`LoopRefused_*`). `if` / ternaries and `switch` still work.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)