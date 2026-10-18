#pragma once

// EffectGraph: ShaderLab's effect library and graph evaluator for any D3D11 or
// Direct2D program.
//
// A context is created from your ID3D11Device; it builds a Direct2D factory,
// device and device context on top of it and evaluates graphs there. Nodes are
// created from the context and wired output-to-input, as D2D effects are. Asking
// for a node's result (Realize, RealizeTo) evaluates only what that node needs,
// re-using what has not changed.
//
//     #include "luminance_statistics.effect.h"     // the built-ins you use
//     #include "ictcp_tone_map_hdr_sdr.effect.h"
//
//     winrt::com_ptr<IEffectGraphContext> context;
//     EffectGraphCreateContext(d3dDevice, nullptr, IID_PPV_ARGS(context.put()));
//
//     winrt::com_ptr<IEffectGraphNode> input, stats, toneMap;
//     context->CreateTextureInput(hdrTexture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put());
//     context->CreateEffectById(CLSID_EffectGraphLuminanceStatistics, stats.put());
//     context->CreateEffectById(CLSID_EffectGraphIctcpToneMapHdrSdr, toneMap.put());
//     stats->SetInput(0, input.get());
//     toneMap->SetInput(0, input.get());
//     toneMap->Bind(L"SourcePeakNits", stats.get(), L"Max");
//
//     context->RealizeTo(toneMap.get(), sdrTexture);      // B8G8R8A8_UNORM_SRGB
//
// The pipeline is linear scRGB: 1.0 is 80 nits, values above 1.0 are HDR, and
// negative components carry wide-gamut colour.
//
// Threading: a context and its nodes are used from one thread at a time, like
// a Direct2D device context. Separate contexts may run on separate threads.
//
// Link EffectGraph.lib and ship EffectGraph.dll; the built-in effect headers
// and EffectGraphCompile come with them. This header stands alone: it needs
// only the Windows SDK, and it does not use the C++ standard library or
// C++/WinRT.

#include <d3d11.h>
#include <d2d1_3.h>

#include "EffectGraphVersion.h"

// EFFECTGRAPH_STATIC: the program links the engine library itself, as
// ShaderLab's own programs do, instead of EffectGraph.dll.
#ifndef EFFECTGRAPH_API
#ifdef EFFECTGRAPH_STATIC
#define EFFECTGRAPH_API extern "C"
#else
#define EFFECTGRAPH_API extern "C" __declspec(dllimport)
#endif
#endif

interface IEffectGraphContext;
interface IEffectGraphNode;
interface IEffectGraphValues;
interface IEffectGraphValuesBuffer;

// What a texture input holds. Anything but linear scRGB is converted on the
// way in, inside the graph.
typedef enum EFFECTGRAPH_INPUT_ENCODING
{
    EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR = 0,   // FP16 / FP32, 1.0 = 80 nits
    EFFECTGRAPH_INPUT_ENCODING_SRGB = 1,           // 8-bit sRGB
    EFFECTGRAPH_INPUT_ENCODING_HDR10 = 2,          // 10-bit PQ, BT.2020 primaries
} EFFECTGRAPH_INPUT_ENCODING;

typedef enum EFFECTGRAPH_CONTEXT_FLAGS
{
    EFFECTGRAPH_CONTEXT_FLAGS_NONE = 0,
    // Compile shaders on background threads. A node whose shader is still
    // compiling has no result, and Realize returns EFFECTGRAPH_E_COMPILE_PENDING.
    // Without it, the first Realize of a node compiles inline, which can take
    // tens of seconds for a large shader with no cache entry.
    EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE = 0x1,
    // Let an effect registered from a blob compile a build its blob lacks.
    // Without it the context uses the blob's generic build for that node and
    // serves its bindings from the CPU: always correct, a readback slower.
    // Needs d3dcompiler_47.dll at run time.
    EFFECTGRAPH_CONTEXT_FLAGS_ALLOW_RUNTIME_COMPILE = 0x2,
} EFFECTGRAPH_CONTEXT_FLAGS;

typedef struct EFFECTGRAPH_CONTEXT_DESC
{
    EFFECTGRAPH_CONTEXT_FLAGS flags;
    // Where compiled shaders are kept between runs. Null keeps them in memory
    // only. Shared by every context in the process; the first context to name
    // one sets it.
    PCWSTR shaderCacheDirectory;
} EFFECTGRAPH_CONTEXT_DESC;

// One output field of an analysis node (Max, Mean, a histogram, ...).
typedef enum EFFECTGRAPH_FIELD_TYPE
{
    EFFECTGRAPH_FIELD_TYPE_FLOAT = 0,
    EFFECTGRAPH_FIELD_TYPE_FLOAT2 = 1,
    EFFECTGRAPH_FIELD_TYPE_FLOAT3 = 2,
    EFFECTGRAPH_FIELD_TYPE_FLOAT4 = 3,
    EFFECTGRAPH_FIELD_TYPE_FLOAT_ARRAY = 4,
    EFFECTGRAPH_FIELD_TYPE_FLOAT2_ARRAY = 5,
    EFFECTGRAPH_FIELD_TYPE_FLOAT3_ARRAY = 6,
    EFFECTGRAPH_FIELD_TYPE_FLOAT4_ARRAY = 7,
} EFFECTGRAPH_FIELD_TYPE;

typedef struct EFFECTGRAPH_FIELD_INFO
{
    EFFECTGRAPH_FIELD_TYPE type;
    UINT32 componentCount;   // 1-4 per element
    UINT32 elementCount;     // 1 for a scalar or vector, the length for an array
    UINT32 bufferElement;    // the float4 element the field starts at in its values buffer
} EFFECTGRAPH_FIELD_INFO;

#define EFFECTGRAPH_INVALID_INDEX 0xFFFFFFFFu

// Errors beyond the usual E_INVALIDARG / E_OUTOFMEMORY.
#define EFFECTGRAPH_E_NODE_REMOVED       MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E01)   // the node was removed from its context
#define EFFECTGRAPH_E_WRONG_CONTEXT      MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E02)   // a node from another context
#define EFFECTGRAPH_E_CYCLE              MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E03)   // the edge or binding would close a loop
#define EFFECTGRAPH_E_UNKNOWN_EFFECT     MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E04)   // no effect of that name or CLSID
#define EFFECTGRAPH_E_COMPILE_PENDING    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E05)   // async compile still running
#define EFFECTGRAPH_E_NODE_FAILED        MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E06)   // see IEffectGraphNode::GetError
#define EFFECTGRAPH_E_UNSUPPORTED_DEVICE MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E07)   // no D3D11_CREATE_DEVICE_BGRA_SUPPORT
#define EFFECTGRAPH_E_UNSUPPORTED_FORMAT MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E08)   // a texture the graph cannot read or write
#define EFFECTGRAPH_E_INCOMPATIBLE_EFFECT MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E09)  // an effect blob built for another shader contract
#define EFFECTGRAPH_E_EFFECT_NOT_INCLUDED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0E0A)  // a built-in whose header the program did not include

// Effects as a header generated by EffectGraphCompile holds them: the
// description and each build, with builds left out by the header's #if
// selection simply absent. Build it from the header; do not fill it by hand.
typedef struct EFFECTGRAPH_BUILD
{
    const void* data;
    UINT32 size;
} EFFECTGRAPH_BUILD;

typedef struct EFFECTGRAPH_EFFECT_SOURCE
{
    UINT32 format;
    UINT64 contractHash;
    const void* description;
    UINT32 descriptionSize;
    const EFFECTGRAPH_BUILD* builds;
    UINT32 buildCount;
} EFFECTGRAPH_EFFECT_SOURCE;

// Creates a context on `device`, which must have been created with
// D3D11_CREATE_DEVICE_BGRA_SUPPORT. Turns on the device's multithread
// protection (ID3D10Multithread): video decode and shader compiles submit from
// other threads. `desc` may be null for the defaults.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphCreateContext(
    ID3D11Device* device,
    const EFFECTGRAPH_CONTEXT_DESC* desc,
    REFIID riid,
    void** context);

// Registers the effects in a blob file written by EffectGraphCompile --out:
// their description and compiled shaders, so CreateEffect can create them by
// name without compiling HLSL. Process-wide; the blob is copied, and each
// build is decompressed the first time a node uses it. Call at startup,
// before creating nodes. S_FALSE when some of its effects were not registered
// (a name or id already taken by a newer version);
// HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) when none were;
// EFFECTGRAPH_E_INCOMPATIBLE_EFFECT when it was built against other shared
// HLSL headers than this engine's; HRESULT_FROM_WIN32(ERROR_INVALID_DATA) when
// it is not a blob or is damaged.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffect(const void* data, UINT32 size);

// Registers the effects a generated header holds, as EffectGraphRegisterEffect
// does for a blob file, but without copying: the header's arrays stay where
// they are, and each build is decompressed the first time a node uses it.
// An effect whose generic build the header's selection left out is not
// registered, and S_FALSE when that leaves none. Otherwise the same results as
// EffectGraphRegisterEffect.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffects(const EFFECTGRAPH_EFFECT_SOURCE* source);

// What a generated header with startup registration calls from a static
// initializer: records `source`, which must stay valid for the life of the
// process, and registers it when the engine's effect list is first used, as
// EffectGraphRegisterEffects would. Safe under the loader lock. Failures are
// written to stderr then, and the effects are simply absent.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphAddEffects(const EFFECTGRAPH_EFFECT_SOURCE* source);

// The development path: adds every effect defined in the .json files in
// `directory` (as ShaderLab's Effect Designer saves them), compiled from their
// HLSL when first used. Process-wide. A file that fails is skipped; the return
// value is S_FALSE when any did.
EFFECTGRAPH_API HRESULT WINAPI EffectGraphLoadEffects(PCWSTR directory);

// Stops the engine's background threads (shader compiles, cache upkeep).
// Call once, after releasing every context, before the DLL unloads.
EFFECTGRAPH_API void WINAPI EffectGraphShutdown();

// Owns a graph, the Direct2D objects it renders with, and its caches.
MIDL_INTERFACE("ec291218-999c-42ae-98e3-e38ae446d40b")
IEffectGraphContext : public IUnknown
{
    // ---- Creating nodes. The context owns every node: releasing a node
    // interface does not remove the node, RemoveNode does.

    // A ShaderLab effect, a loaded user effect, or a Direct2D built-in, by the
    // name ShaderLab shows ("Luminance Statistics", "Gaussian Blur"). A
    // ShaderLab built-in exists only when the program includes its header
    // (luminance_statistics.effect.h, or EffectGraphBuiltIns.h for all of
    // them); without it, EFFECTGRAPH_E_EFFECT_NOT_INCLUDED.
    STDMETHOD(CreateEffect)(PCWSTR name, IEffectGraphNode** node) PURE;

    // A ShaderLab effect by the class id its generated header defines
    // (CLSID_EffectGraphLuminanceStatistics), so a missing header is a
    // compile error rather than a run-time one.
    STDMETHOD(CreateEffectById)(REFCLSID effectId, IEffectGraphNode** node) PURE;

    // Any Direct2D effect class registered on the context's factory, by CLSID.
    // Register your own classes first through GetD2DDeviceContext's factory.
    STDMETHOD(CreateD2DEffect)(REFCLSID effectId, IEffectGraphNode** node) PURE;

    // A node whose output is `texture`, read in place. It must be a 2D texture
    // on this context's device with D3D11_BIND_SHADER_RESOURCE, one mip level
    // and one sample. Change the texture's contents and call
    // IEffectGraphInput::Invalidate; swap textures with SetTexture.
    STDMETHOD(CreateTextureInput)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding,
                                  IEffectGraphNode** node) PURE;

    // A node whose output is any Direct2D image on this context's device: a
    // bitmap, a command list, or the output of your own effect chain.
    STDMETHOD(CreateImageInput)(ID2D1Image* image, IEffectGraphNode** node) PURE;

    // Removes a node with its edges and bindings. Interfaces to it then return
    // EFFECTGRAPH_E_NODE_REMOVED.
    STDMETHOD(RemoveNode)(IEffectGraphNode* node) PURE;

    // ---- Evaluating. Each call evaluates the nodes `node` depends on that
    // changed since the last call, runs any compute work they need at once,
    // and returns. A failure in an upstream node returns
    // EFFECTGRAPH_E_NODE_FAILED; GetError on the failing node says why. The
    // context evaluates on a device context of its own, so these may be
    // called at any time, including while your device context is drawing.

    // The node's output as a Direct2D image on this context's D2D device
    // (GetD2DDevice): draw it with any device context you create on that
    // device. Pixel shaders run when the image is drawn, as with any D2D
    // effect; compute work has already run. Valid until the graph next
    // changes. For a node with no image output, such as statistics, read its
    // fields with IEffectGraphNode::GetValues instead.
    STDMETHOD(Realize)(IEffectGraphNode* node, ID2D1Image** image) PURE;

    // Renders the node's output into `target`, top-left at (0, 0), and flushes,
    // so D3D11 work you submit next sees the result. The target needs
    // D3D11_BIND_RENDER_TARGET and a format Direct2D can draw to; a
    // B8G8R8A8_UNORM_SRGB target receives sRGB-encoded 8-bit output.
    STDMETHOD(RealizeTo)(IEffectGraphNode* node, ID3D11Texture2D* target) PURE;

    // ---- Interop.

    STDMETHOD_(void, GetD3DDevice)(ID3D11Device** device) PURE;

    // The Direct2D device the context built on your D3D11 device. Create your
    // own device contexts on it to draw Realize's images, or to make bitmaps
    // and images for CreateImageInput.
    STDMETHOD_(void, GetD2DDevice)(ID2D1Device** device) PURE;

    // ---- Losing the device.
    //
    // When the device is removed (a driver update or reset, a GPU removed or
    // hung), Realize, RealizeTo and GetValues return the device's removed reason,
    // such as DXGI_ERROR_DEVICE_REMOVED or DXGI_ERROR_DEVICE_RESET, from then
    // on. Create a new device and move the context onto it: the graph, node
    // interfaces and property values are kept, effects rebuild and sources
    // reopen on the next evaluation. Input nodes are empty until SetTexture or
    // SetImage gives them something on the new device, and images from
    // Realize and device contexts on the old GetD2DDevice belong to the old
    // device. The same call moves a working context to another adapter.
    // Fails with the context unchanged when the new device cannot be used
    // (EFFECTGRAPH_E_UNSUPPORTED_DEVICE without BGRA support).
    STDMETHOD(MoveToDevice)(ID3D11Device* device) PURE;
};

// A node in a context's graph. Its properties are read and written through
// ID2D1Properties, as a Direct2D effect's are: by index or name, typed
// (D2D1_PROPERTY_TYPE). Setting a property marks the node changed.
MIDL_INTERFACE("4249bbf5-c7df-4e7b-8ef5-d4144793362d")
IEffectGraphNode : public ID2D1Properties
{
    STDMETHOD_(void, GetContext)(IEffectGraphContext** context) PURE;

    // Connects `input`'s output to input `index` of this node; null
    // disconnects it. Fails with EFFECTGRAPH_E_CYCLE rather than close a loop.
    STDMETHOD(SetInput)(UINT32 index, IEffectGraphNode* input) PURE;
    STDMETHOD_(void, GetInput)(UINT32 index, IEffectGraphNode** input) PURE;
    STDMETHOD_(UINT32, GetInputCount)() PURE;

    // Drives `property` from analysis field `field` of `source`: each time the
    // source is evaluated, the property takes the field's value. Vector
    // properties take the field's components in order. The value stays on
    // the GPU where it can.
    STDMETHOD(Bind)(PCWSTR property, IEffectGraphNode* source, PCWSTR field) PURE;
    STDMETHOD(Unbind)(PCWSTR property) PURE;

    // The node's analysis fields, evaluating it first if anything it depends
    // on changed; empty for a node with none. Fails as Realize does when the
    // node cannot be evaluated, with no values.
    STDMETHOD(GetValues)(IEffectGraphValues** values) PURE;

    // The node's analysis fields on the GPU (IEffectGraphValuesBuffer).
    // Evaluates the node first, as GetValues does, but reads nothing back to
    // the CPU, so nothing waits for the GPU. S_FALSE and null for a node with
    // no fields.
    STDMETHOD(GetValuesBuffer)(IEffectGraphValuesBuffer** buffer) PURE;

    // Why the node has no result, in its last evaluation: a compile error, a
    // missing input, an unreadable file. Empty when it has none.
    // GetErrorLength excludes the terminator.
    STDMETHOD_(UINT32, GetErrorLength)() PURE;
    STDMETHOD(GetError)(PWSTR buffer, UINT32 bufferCount) PURE;

    // The name ShaderLab shows for the node; unique names are not enforced.
    STDMETHOD_(UINT32, GetNameLength)() PURE;
    STDMETHOD(GetName)(PWSTR buffer, UINT32 bufferCount) PURE;
    STDMETHOD(SetName)(PCWSTR name) PURE;
};

// A node created by CreateTextureInput or CreateImageInput; QueryInterface a
// node for it.
MIDL_INTERFACE("c50a93e4-aef5-41c7-bdde-ec065366cb1c")
IEffectGraphInput : public IEffectGraphNode
{
    // Something else wrote new contents into the texture or image this node
    // reads: downstream nodes are evaluated again on the next Realize.
    STDMETHOD_(void, Invalidate)() PURE;

    // Swaps what the node reads. A texture input takes a texture and an image
    // input an image; the other call fails with E_NOT_VALID_STATE.
    STDMETHOD(SetTexture)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding) PURE;
    STDMETHOD(SetImage)(ID2D1Image* image) PURE;
};

// A node's analysis fields where the GPU holds them: a StructuredBuffer<float4>
// on the context's device, for your own shaders and D3D11 calls. Fields follow
// one another in GetValues' order, each from its
// EFFECTGRAPH_FIELD_INFO::bufferElement: a scalar or vector in one element (x,
// xy, xyz, xyzw), a FLOAT_ARRAY four values to an element, any other array one
// element per entry. A compute node's own results buffer is handed out as is;
// a node whose values are worked out on the CPU (Clock, Numeric Expression,
// parameters) gets them uploaded into a buffer the context keeps.
//
// The buffer holds the values of the evaluation GetValuesBuffer ran until the
// node is next evaluated, which writes it again: in place for a compute node,
// on the next GetValuesBuffer for any other. D3D11 work you submit on the
// device's immediate context sees what was written before it. Ask the node
// again after each evaluation; the buffer can change when its fields do.
MIDL_INTERFACE("d8816103-9483-4cfa-86c2-5f2a88b1e52b")
IEffectGraphValuesBuffer : public IUnknown
{
    // The layout, as IEffectGraphValues gives it.
    STDMETHOD_(UINT32, GetFieldCount)() PURE;
    STDMETHOD_(UINT32, GetFieldIndex)(PCWSTR name) PURE;
    STDMETHOD(GetFieldInfo)(UINT32 index, EFFECTGRAPH_FIELD_INFO* info) PURE;

    // The buffer and a shader resource view of all of it, for any D3D11 call.
    STDMETHOD_(void, GetBuffer)(ID3D11Buffer** buffer) PURE;
    STDMETHOD_(void, GetView)(ID3D11ShaderResourceView** view) PURE;

    // The buffer's contents now, on the CPU. Waits for the GPU work that
    // writes it: call it when you need the numbers, not every frame.
    STDMETHOD(ReadValues)(IEffectGraphValues** values) PURE;

    // Replaces field `index` with componentCount * elementCount floats, in
    // GetFloats' order, until the node is next evaluated. Everything in the
    // graph that reads the field sees the new value: nodes bound to it are
    // evaluated again, whether they read it from this buffer, from the copy
    // pixel shaders sample, or as a constant. Fails with E_INVALIDARG for any
    // other float count, and EFFECTGRAPH_E_NODE_REMOVED once the node is gone.
    STDMETHOD(SetFloats)(UINT32 index, const float* values, UINT32 valueCount) PURE;
};

// A snapshot of a node's analysis fields, taken when GetValues was called.
MIDL_INTERFACE("794df4a5-0e0a-495b-954b-619effb74d90")
IEffectGraphValues : public IUnknown
{
    STDMETHOD_(UINT32, GetFieldCount)() PURE;

    // EFFECTGRAPH_INVALID_INDEX when there is no field of that name.
    STDMETHOD_(UINT32, GetFieldIndex)(PCWSTR name) PURE;

    STDMETHOD(GetFieldName)(UINT32 index, PWSTR buffer, UINT32 bufferCount) PURE;
    STDMETHOD(GetFieldInfo)(UINT32 index, EFFECTGRAPH_FIELD_INFO* info) PURE;

    // The field's components, element after element: componentCount *
    // elementCount floats. A smaller buffer gets the first floats and
    // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER).
    STDMETHOD(GetFloats)(UINT32 index, float* values, UINT32 valueCount) PURE;

    // The first component of a field by name: GetFloat(L"Max", &peak).
    STDMETHOD(GetFloat)(PCWSTR name, float* value) PURE;
};
