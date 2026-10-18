# Newly Added Effect Default Properties

This document lists the D2D built-in effects that previously had **no default properties** in the
`EffectRegistry` and now have full defaults + metadata (min/max ranges, UI hints, enum labels).
Use this as a reference when updating the main documentation.

---

## Brightness
**Category:** Color | **CLSID:** `CLSID_D2D1Brightness`

| Property     | Type   | Default      | Range          | Notes                             |
|-------------|--------|-------------|----------------|-----------------------------------|
| WhitePoint  | float2 | (1.0, 1.0)  | [0.0, 1.0]    | Controls the white point curve    |
| BlackPoint  | float2 | (0.0, 0.0)  | [0.0, 1.0]    | Controls the black point curve    |

---

## Border
**Category:** Transform | **CLSID:** `CLSID_D2D1Border`

| Property    | Type   | Default | Values                     |
|------------|--------|---------|----------------------------|
| EdgeModeX  | enum   | Clamp   | Clamp, Wrap, Mirror        |
| EdgeModeY  | enum   | Clamp   | Clamp, Wrap, Mirror        |

---

## Crop
**Category:** Transform | **CLSID:** `CLSID_D2D1Crop`

| Property | Type   | Default                          | Notes                                     |
|---------|--------|----------------------------------|-------------------------------------------|
| Rect    | float4 | (-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX) | Left, Top, Right, Bottom in pixels |

---

## Posterize
**Category:** Detail | **CLSID:** `CLSID_D2D1Posterize`

| Property         | Type   | Default | Range      | Notes                    |
|-----------------|--------|---------|------------|--------------------------|
| RedValueCount   | uint32 | 4       | [2, 16]    | Posterization levels (R) |
| GreenValueCount | uint32 | 4       | [2, 16]    | Posterization levels (G) |
| BlueValueCount  | uint32 | 4       | [2, 16]    | Posterization levels (B) |

---

## Spot Diffuse
**Category:** Lighting | **CLSID:** `CLSID_D2D1SpotDiffuse`

| Property          | Type   | Default          | Range              | Notes                         |
|------------------|--------|------------------|--------------------|-------------------------------|
| LightPosition    | float3 | (0, 0, 100)     | [-10000, 10000]    | 3D position of the light      |
| PointsAt         | float3 | (0, 0, 0)       | [-10000, 10000]    | Direction the spot points at  |
| Focus            | float  | 1.0              | [0, 200]           | Focus exponent of the spot    |
| LimitingConeAngle| float  | 90.0             | [0, 90]            | Half-angle of the cone (deg)  |
| DiffuseConstant  | float  | 1.0              | [0, 10000]         | Kd reflectance coefficient    |
| SurfaceScale     | float  | 1.0              | [0, 10000]         | Surface height scale factor   |
| Color            | float3 | (1, 1, 1)       | —                  | Light color (scRGB)           |

---

## Spot Specular
**Category:** Lighting | **CLSID:** `CLSID_D2D1SpotSpecular`

| Property          | Type   | Default          | Range              | Notes                          |
|------------------|--------|------------------|--------------------|--------------------------------|
| LightPosition    | float3 | (0, 0, 100)     | [-10000, 10000]    | 3D position of the light       |
| PointsAt         | float3 | (0, 0, 0)       | [-10000, 10000]    | Direction the spot points at   |
| Focus            | float  | 1.0              | [0, 200]           | Focus exponent of the spot     |
| LimitingConeAngle| float  | 90.0             | [0, 90]            | Half-angle of the cone (deg)   |
| SpecularExponent | float  | 1.0              | [1, 128]           | Shininess exponent             |
| SpecularConstant | float  | 1.0              | [0, 10000]         | Ks reflectance coefficient     |
| SurfaceScale     | float  | 1.0              | [0, 10000]         | Surface height scale factor    |
| Color            | float3 | (1, 1, 1)       | —                  | Light color (scRGB)            |

---

## Distant Diffuse
**Category:** Lighting | **CLSID:** `CLSID_D2D1DistantDiffuse`

| Property         | Type   | Default | Range          | Notes                           |
|-----------------|--------|---------|----------------|---------------------------------|
| Azimuth         | float  | 0.0     | [0, 360]       | Light direction angle (degrees) |
| Elevation       | float  | 0.0     | [-90, 90]      | Light elevation angle (degrees) |
| DiffuseConstant | float  | 1.0     | [0, 10000]     | Kd reflectance coefficient      |
| SurfaceScale    | float  | 1.0     | [0, 10000]     | Surface height scale factor     |
| Color           | float3 | (1,1,1) | —              | Light color (scRGB)             |

---

## Distant Specular
**Category:** Lighting | **CLSID:** `CLSID_D2D1DistantSpecular`

| Property          | Type   | Default | Range          | Notes                           |
|------------------|--------|---------|----------------|---------------------------------|
| Azimuth          | float  | 0.0     | [0, 360]       | Light direction angle (degrees) |
| Elevation        | float  | 0.0     | [-90, 90]      | Light elevation angle (degrees) |
| SpecularExponent | float  | 1.0     | [1, 128]       | Shininess exponent              |
| SpecularConstant | float  | 1.0     | [0, 10000]     | Ks reflectance coefficient      |
| SurfaceScale     | float  | 1.0     | [0, 10000]     | Surface height scale factor     |
| Color            | float3 | (1,1,1) | —              | Light color (scRGB)             |

---

## Morphology
**Category:** Distort | **CLSID:** `CLSID_D2D1Morphology`

| Property | Type   | Default | Range      | Values          |
|---------|--------|---------|------------|-----------------|
| Mode    | enum   | 0       | —          | Erode, Dilate   |
| Width   | uint32 | 1       | [1, 100]   | Kernel width    |
| Height  | uint32 | 1       | [1, 100]   | Kernel height   |

---

## Histogram
**Category:** Analysis | **CLSID:** `CLSID_D2D1Histogram`

| Property       | Type   | Default | Range        | Values                     |
|---------------|--------|---------|--------------|----------------------------|
| ChannelSelect | enum   | 3       | —            | Red, Green, Blue, Alpha    |
| NumBins       | uint32 | 256     | [2, 1024]    | Number of histogram bins   |

---

## Tile
**Category:** Transform | **CLSID:** `CLSID_D2D1Tile`

| Property | Type   | Default             | Notes                                   |
|---------|--------|---------------------|-----------------------------------------|
| Rect    | float4 | (0, 0, 100, 100)   | Left, Top, Right, Bottom — tile region  |

---

## Gamma Transfer
**Category:** Color | **CLSID:** `CLSID_D2D1GammaTransfer`

Per-channel transfer: `output = Amplitude * input^Exponent + Offset`

| Property        | Type  | Default | Range        | Notes                           |
|----------------|-------|---------|--------------|-------------------------------- |
| RedAmplitude   | float | 1.0     | [0, 10]      | Red channel amplitude           |
| RedExponent    | float | 1.0     | [0, 10]      | Red channel exponent            |
| RedOffset      | float | 0.0     | [-1, 1]      | Red channel offset              |
| RedDisable     | bool  | false   | —            | Bypass red channel              |
| GreenAmplitude | float | 1.0     | [0, 10]      | Green channel amplitude         |
| GreenExponent  | float | 1.0     | [0, 10]      | Green channel exponent          |
| GreenOffset    | float | 0.0     | [-1, 1]      | Green channel offset            |
| GreenDisable   | bool  | false   | —            | Bypass green channel            |
| BlueAmplitude  | float | 1.0     | [0, 10]      | Blue channel amplitude          |
| BlueExponent   | float | 1.0     | [0, 10]      | Blue channel exponent           |
| BlueOffset     | float | 0.0     | [-1, 1]      | Blue channel offset             |
| BlueDisable    | bool  | false   | —            | Bypass blue channel             |
| AlphaAmplitude | float | 1.0     | [0, 10]      | Alpha channel amplitude         |
| AlphaExponent  | float | 1.0     | [0, 10]      | Alpha channel exponent          |
| AlphaOffset    | float | 0.0     | [-1, 1]      | Alpha channel offset            |
| AlphaDisable   | bool  | false   | —            | Bypass alpha channel            |

---

## Point Diffuse (previously partial — Color added)
**Category:** Lighting | **CLSID:** `CLSID_D2D1PointDiffuse`

Added property:

| Property | Type   | Default  | Notes           |
|---------|--------|----------|-----------------|
| Color   | float3 | (1,1,1)  | Light color     |

---

## Point Specular (previously partial — Color added)
**Category:** Lighting | **CLSID:** `CLSID_D2D1PointSpecular`

Added property:

| Property | Type   | Default  | Notes           |
|---------|--------|----------|-----------------|
| Color   | float3 | (1,1,1)  | Light color     |

---

## Effects Still Without Editable Properties

These effects have complex property types that don't fit the `PropertyValue` variant:

| Effect              | Reason                                              |
|--------------------|-----------------------------------------------------|
| 2D Affine Transform | 3×2 matrix (D2D1_MATRIX_3X2_F)                     |
| Convolve Matrix     | Variable-size kernel matrix                          |
| 3D Lookup Table     | Requires a LUT texture resource                      |
| Grayscale           | No parameters (pass-through effect)                  |
| Invert              | No parameters (pass-through effect)                  |
| Alpha Mask          | No parameters (compositing-only)                     |
| Premultiply         | No parameters (format conversion)                    |
| Unpremultiply       | No parameters (format conversion)                    |

---

## Phase 2 Additions

### Color Matrix
**Category:** Color | **CLSID:** `CLSID_D2D1ColorMatrix`

Now has full 5×4 matrix editor (D2D1_MATRIX_5X4_F type added to PropertyValue variant).

| Property    | Type            | Default   | UI Control                  |
|------------|-----------------|-----------|------------------------------|
| ColorMatrix | D2D1_MATRIX_5X4_F | Identity | 5×4 grid of NumberBoxes     |

Default identity matrix:
```
1  0  0  0
0  1  0  0
0  0  1  0
0  0  0  1
0  0  0  0
```

### Table Transfer
**Category:** Color | **CLSID:** `CLSID_D2D1TableTransfer`

Now has per-channel LUT arrays (std::vector<float> type added to PropertyValue variant) 
with a draggable curve editor in a popup dialog.

| Property      | Type            | Default              | UI Control             |
|--------------|-----------------|----------------------|------------------------|
| RedTable     | float array     | 256-entry identity   | Curve editor flyout    |
| GreenTable   | float array     | 256-entry identity   | Curve editor flyout    |
| BlueTable    | float array     | 256-entry identity   | Curve editor flyout    |
| AlphaTable   | float array     | 256-entry identity   | Curve editor flyout    |
| RedDisable   | bool            | false                | ToggleSwitch           |
| GreenDisable | bool            | false                | ToggleSwitch           |
| BlueDisable  | bool            | false                | ToggleSwitch           |
| AlphaDisable | bool            | false                | ToggleSwitch           |

---

# Analysis/Compute Effect Output Convention

## Overview

Some D2D effects (like Histogram) and future custom compute effects produce **data outputs**
rather than images. ShaderLab's evaluator detects these effects and reads back their output
data after graph evaluation. The data is stored in `EffectNode::analysisOutput` and
visualized in the Properties panel.

## Built-in Analysis Effects

### Histogram
- **Output type:** `AnalysisOutputType::Histogram`
- **Data:** Float array of `NumBins` values (default 256)
- **Readback:** `effect->GetValue(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT, D2D1_PROPERTY_TYPE_BLOB, ...)`
- **Visualization:** Color-coded bar chart in the Properties panel
- **Channel:** Determined by the `ChannelSelect` property (R=0, G=1, B=2, A=3)

## Convention for Custom Compute Effects

Custom compute effects that produce non-image outputs should follow this convention:

### 1. Declare Output Type
Set `AnalysisOutputType` on the node after evaluation:
```cpp
node.analysisOutput.type = AnalysisOutputType::Histogram;
node.analysisOutput.data = { ... };  // float array
node.analysisOutput.label = L"My Analysis";
node.analysisOutput.channelIndex = 0;  // optional, for per-channel data
```

### 2. Output Types
| Type          | Description                                    | Visualization        |
|--------------|------------------------------------------------|----------------------|
| `Histogram`  | Float array of bin counts/weights              | Bar chart            |
| `Typed`      | Named, typed fields (`analysisFields`)         | Properties panel, data pins |

### 3. Readback Pattern
After forcing D2D to compute the effect (by drawing its output to a temp target),
read the output property:
```cpp
effect->GetValue(outputPropertyIndex, D2D1_PROPERTY_TYPE_BLOB,
    reinterpret_cast<BYTE*>(buffer.data()), bufferSizeInBytes);
```

### 4. Future Extensions
- `AnalysisOutputType::Texture2D` — for compute effects that produce 2D data
- `AnalysisOutputType::StructuredBuffer` — for effects with structured output
- Custom visualization callbacks via effect metadata

