# Effect Versioning System

Each ShaderLab effect carries an `effectId` (stable GUID) and `effectVersion` (integer). This enables safe upgrades when effects are updated with new parameters or shader changes.

- **`effectId`**: Stable identifier that survives effect renames.
- **`effectVersion`**: Monotonically increasing version per effect.
- **Per-node "Update Effect" button**: Shown in Properties panel when a node's version is behind the registry.
- **Engine-side**: `Effects/EffectUpgrade` holds the rule both buttons use: `NewerDescriptorFor(node)` finds a newer descriptor, `UpgradeNode` swaps in its definition, pins and properties while keeping the values of properties that still exist, and `UpgradeOutdatedNodes` does every node. Any host can call them on the render thread.
- **"Update Effects (N)" batch button**: Toolbar button to upgrade all outdated nodes at once.
- **Property preservation**: On upgrade, existing user property values are preserved where parameter names match.
- **Renamed ids**: `ShaderLabEffects::FindById` resolves a retired id through the `scLegacyEffectIds` table (`Perceptual Gamut Map` -> `ICtCp Gamut Map`), so a graph saved with the old id loads and offers the update.
- **Version guard**: `TestEffectVersionGuard` pins every built-in's `effectVersion` beside a fingerprint of its HLSL (comments and whitespace removed), inputs, parameter names, types, defaults, enum labels and analysis fields (decision #203). Changing any of those fails the test with the row to paste; bump `effectVersion` first if saved graphs should be offered the change. A comment-only HLSL edit leaves the fingerprint unchanged. `LibraryVersion()` is the sum of the built-in versions, so it rises with any bump but does not identify a catalog.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)