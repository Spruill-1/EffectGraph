# Pipeline Format Strategy

The rendering pipeline always uses **scRGB FP16** (`DXGI_FORMAT_R16G16B16A16_FLOAT` with `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). Linear floating-point preserves full HDR range and precision; scRGB covers the full BT.2020 gamut including negative values. DWM/ACM handles the final display conversion to whatever the connected monitor reports. There is no fixed built-in tone mapper in the render path — users build tone mappers as graph effects (e.g. via the `D2D ColorMatrix`/`HdrToneMap`/`WhiteLevelAdjustment` chain), which keeps full HDR accuracy on by default and avoids accidental clipping.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)