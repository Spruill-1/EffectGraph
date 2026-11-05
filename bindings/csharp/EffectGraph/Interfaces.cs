// The EffectGraph COM interfaces (Engine/Api/EffectGraph.h), method for method
// in vtable order. Direct3D and Direct2D objects cross as IntPtr, so these work
// with any wrapper (Vortice, SharpDX, CsWin32) or with raw pointers.
using System;
using System.Runtime.InteropServices;

namespace EffectGraph;

public enum InputEncoding
{
    ScRgbLinear = 0,   // FP16 / FP32, 1.0 = 80 nits
    Srgb = 1,          // 8-bit sRGB
    Hdr10 = 2,         // 10-bit PQ, BT.2020 primaries
}

[Flags]
public enum ContextFlags
{
    None = 0,
    AsyncCompile = 0x1,
    AllowRuntimeCompile = 0x2,
    // 1.1: a frame that would run a fallback build fails with
    // HResults.FallbackBuild and draws nothing.
    NoFallback = 0x4,
    // 1.1: bound analysis fields reach their consumers through a deferred
    // commit, under each binding's CommitPolicy.
    DeferredAnalysisCommit = 0x8,
}

// EFFECTGRAPH_COMMIT_POLICY (1.1): how a bound field reaches its consumers
// under ContextFlags.DeferredAnalysisCommit.
public enum CommitPolicy
{
    Exact = 0,
    Immediate = 1,
    Threshold = 2,
    Hosted = 3,
}

public enum FieldType
{
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    FloatArray = 4,
    Float2Array = 5,
    Float3Array = 6,
    Float4Array = 7,
}

[StructLayout(LayoutKind.Sequential)]
public struct FieldInfo
{
    public FieldType Type;
    public uint ComponentCount;   // 1-4 per element
    public uint ElementCount;     // 1 for a scalar or vector, the length for an array
    public uint BufferElement;    // the float4 element the field starts at in its values buffer
}

// D2D1_PROPERTY_TYPE, for the ID2D1Properties methods.
public enum PropertyType : uint
{
    Unknown = 0, String = 1, Bool = 2, UInt32 = 3, Int32 = 4, Float = 5, Vector2 = 6, Vector3 = 7,
    Vector4 = 8, Blob = 9, IUnknown = 10, Enum = 11, Array = 12, Clsid = 13, Matrix3x2 = 14,
    Matrix4x3 = 15, Matrix4x4 = 16, Matrix5x4 = 17, ColorContext = 18,
}

[ComImport, Guid("ec291218-999c-42ae-98e3-e38ae446d40b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphContext
{
    IEffectGraphNode CreateEffect([MarshalAs(UnmanagedType.LPWStr)] string name);
    IEffectGraphNode CreateEffectById(in Guid effectId);
    IEffectGraphNode CreateD2DEffect(in Guid effectId);
    IEffectGraphNode CreateTextureInput(IntPtr texture, InputEncoding encoding);
    IEffectGraphNode CreateImageInput(IntPtr image);
    void RemoveNode(IEffectGraphNode node);
    // An ID2D1Image* with a reference added: release it with Marshal.Release.
    IntPtr Realize(IEffectGraphNode node);
    void RealizeTo(IEffectGraphNode node, IntPtr target);
    // An ID3D11Device* / ID2D1Device* with a reference added.
    [PreserveSig] void GetD3DDevice(out IntPtr device);
    [PreserveSig] void GetD2DDevice(out IntPtr device);
    void MoveToDevice(IntPtr device);
}

[ComImport, Guid("4249bbf5-c7df-4e7b-8ef5-d4144793362d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphNode
{
    // ID2D1Properties
    [PreserveSig] uint GetPropertyCount();
    void GetPropertyName(uint index, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] char[] name, uint nameCount);
    [PreserveSig] uint GetPropertyNameLength(uint index);
    [PreserveSig] PropertyType GetType(uint index);
    [PreserveSig] uint GetPropertyIndex([MarshalAs(UnmanagedType.LPWStr)] string name);
    void SetValueByName([MarshalAs(UnmanagedType.LPWStr)] string name, PropertyType type, IntPtr data, uint dataSize);
    void SetValue(uint index, PropertyType type, IntPtr data, uint dataSize);
    void GetValueByName([MarshalAs(UnmanagedType.LPWStr)] string name, PropertyType type, IntPtr data, uint dataSize);
    void GetValue(uint index, PropertyType type, IntPtr data, uint dataSize);
    [PreserveSig] uint GetValueSize(uint index);
    IntPtr GetSubProperties(uint index);

    // IEffectGraphNode
    [PreserveSig] void GetContext(out IEffectGraphContext context);
    void SetInput(uint index, IEffectGraphNode? input);
    [PreserveSig] void GetInput(uint index, out IEffectGraphNode? input);
    [PreserveSig] uint GetInputCount();
    void Bind([MarshalAs(UnmanagedType.LPWStr)] string property, IEffectGraphNode source, [MarshalAs(UnmanagedType.LPWStr)] string field);
    void Unbind([MarshalAs(UnmanagedType.LPWStr)] string property);
    IEffectGraphValues GetValues();
    [PreserveSig] int GetValuesBuffer(out IEffectGraphValuesBuffer? buffer);
    [PreserveSig] uint GetErrorLength();
    void GetError([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] buffer, uint bufferCount);
    [PreserveSig] uint GetNameLength();
    void GetName([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] buffer, uint bufferCount);
    void SetName([MarshalAs(UnmanagedType.LPWStr)] string name);
}

[ComImport, Guid("c50a93e4-aef5-41c7-bdde-ec065366cb1c"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphInput
{
    // ID2D1Properties and IEffectGraphNode, which this interface extends:
    // reach them through IEffectGraphNode on the same object.
    [PreserveSig] uint _GetPropertyCount();
    void _GetPropertyName(); void _GetPropertyNameLength(); void _GetType(); void _GetPropertyIndex();
    void _SetValueByName(); void _SetValue(); void _GetValueByName(); void _GetValue(); void _GetValueSize();
    void _GetSubProperties();
    void _GetContext(); void _SetInput(); void _GetInput(); void _GetInputCount(); void _Bind(); void _Unbind();
    void _GetValues(); void _GetValuesBuffer(); void _GetErrorLength(); void _GetError(); void _GetNameLength();
    void _GetName(); void _SetName();

    [PreserveSig] void Invalidate();
    void SetTexture(IntPtr texture, InputEncoding encoding);
    void SetImage(IntPtr image);
}

[ComImport, Guid("d8816103-9483-4cfa-86c2-5f2a88b1e52b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphValuesBuffer
{
    [PreserveSig] uint GetFieldCount();
    [PreserveSig] uint GetFieldIndex([MarshalAs(UnmanagedType.LPWStr)] string name);
    FieldInfo GetFieldInfo(uint index);
    // An ID3D11Buffer* / ID3D11ShaderResourceView* with a reference added.
    [PreserveSig] void GetBuffer(out IntPtr buffer);
    [PreserveSig] void GetView(out IntPtr view);
    IEffectGraphValues ReadValues();
    void SetFloats(uint index, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] float[] values, uint valueCount);
}

[ComImport, Guid("794df4a5-0e0a-495b-954b-619effb74d90"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphValues
{
    [PreserveSig] uint GetFieldCount();
    [PreserveSig] uint GetFieldIndex([MarshalAs(UnmanagedType.LPWStr)] string name);
    void GetFieldName(uint index, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] char[] buffer, uint bufferCount);
    FieldInfo GetFieldInfo(uint index);
    void GetFloats(uint index, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] float[] values, uint valueCount);
    float GetFloat([MarshalAs(UnmanagedType.LPWStr)] string name);
}

// ---- EffectGraph 1.1: dirty rects. New interfaces, reached by casting (QueryInterface)
// a 1.0 one; an object that does not implement one throws InvalidCastException
// (E_NOINTERFACE).

// RealizeToIncremental's flags.
[Flags]
public enum RealizeFlags : uint
{
    None = 0,
    // The target holds this node's previous realize: draw only what changed.
    Incremental = 0x1,
}

// RECT: texel space, top-left origin, right and bottom exclusive.
[StructLayout(LayoutKind.Sequential)]
public struct Rect
{
    public int Left, Top, Right, Bottom;
    public Rect(int left, int top, int right, int bottom) { Left = left; Top = top; Right = right; Bottom = bottom; }
}

// EFFECTGRAPH_MOVE_RECT, laid out as DXGI_OUTDUPL_MOVE_RECT.
[StructLayout(LayoutKind.Sequential)]
public struct MoveRect
{
    public int SourceX, SourceY;
    public Rect Destination;
}

[ComImport, Guid("c8f9cd62-f976-44af-9486-4e5ab237695f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphNode2
{
    // ID2D1Properties and IEffectGraphNode, which this interface extends:
    // reach them through IEffectGraphNode on the same object.
    [PreserveSig] uint _GetPropertyCount();
    void _GetPropertyName(); void _GetPropertyNameLength(); void _GetType(); void _GetPropertyIndex();
    void _SetValueByName(); void _SetValue(); void _GetValueByName(); void _GetValue(); void _GetValueSize();
    void _GetSubProperties();
    void _GetContext(); void _SetInput(); void _GetInput(); void _GetInputCount(); void _Bind(); void _Unbind();
    void _GetValues(); void _GetValuesBuffer(); void _GetErrorLength(); void _GetError(); void _GetNameLength();
    void _GetName(); void _SetName();

    // S_OK, or HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) with count set
    // when capacity is too small. whole is nonzero when everything changed.
    [PreserveSig] int GetInvalidRects([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] Rect[]? rects,
                                      uint capacity, out uint count, out int whole);
    // Deferred analysis commit (ContextFlags.DeferredAnalysisCommit); without
    // it, E_NOT_VALID_STATE. TryGetValues and GetCommittedValues give S_FALSE
    // and null until there is something to return. GetCommittedValues also
    // gives S_FALSE with values while a dispatch's results are not yet read
    // back (see EffectGraph.h).
    [PreserveSig] int TryGetValues(out IEffectGraphValues? values);
    [PreserveSig] int CommitFields([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] uint[] fieldIndices,
                                   [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] float[] values, uint count);
    [PreserveSig] int GetCommittedValues(out IEffectGraphValues? values);
    // E_INVALIDARG when the property is not bound.
    [PreserveSig] int SetCommitPolicy([MarshalAs(UnmanagedType.LPWStr)] string property, CommitPolicy policy);
}

[ComImport, Guid("28f9b611-b610-4415-b631-46bf8d512821"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphInput2
{
    // ID2D1Properties, IEffectGraphNode and IEffectGraphInput, which this
    // interface extends: reach them through those interfaces on the same object.
    [PreserveSig] uint _GetPropertyCount();
    void _GetPropertyName(); void _GetPropertyNameLength(); void _GetType(); void _GetPropertyIndex();
    void _SetValueByName(); void _SetValue(); void _GetValueByName(); void _GetValue(); void _GetValueSize();
    void _GetSubProperties();
    void _GetContext(); void _SetInput(); void _GetInput(); void _GetInputCount(); void _Bind(); void _Unbind();
    void _GetValues(); void _GetValuesBuffer(); void _GetErrorLength(); void _GetError(); void _GetNameLength();
    void _GetName(); void _SetName();
    void _Invalidate(); void _SetTexture(); void _SetImage();

    // S_FALSE, changing nothing, when every rect is empty after clipping.
    [PreserveSig] int InvalidateRects([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] Rect[] rects, uint count);
    // Before the frame's InvalidateRects; S_FALSE when every move clips to nothing.
    [PreserveSig] int MoveRects([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] MoveRect[] moves, uint count);
}

[ComImport, Guid("265e45f3-4f92-4319-8e64-b05e32f3ff4d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IEffectGraphContext2
{
    // IEffectGraphContext, which this interface extends: reach it through
    // IEffectGraphContext on the same object.
    void _CreateEffect(); void _CreateEffectById(); void _CreateD2DEffect(); void _CreateTextureInput();
    void _CreateImageInput(); void _RemoveNode(); void _Realize(); void _RealizeTo(); void _GetD3DDevice();
    void _GetD2DDevice(); void _MoveToDevice();

    // S_OK or S_FALSE (an analysis readback not collected yet, under
    // ContextFlags.DeferredAnalysisCommit). drawn gets at most drawnCapacity rects (at
    // least 1); moved may be null.
    [PreserveSig] int RealizeToIncremental(IEffectGraphNode node, IntPtr target, RealizeFlags flags,
                                           [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] Rect[] drawn,
                                           uint drawnCapacity, out uint drawnCount,
                                           [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 7)] MoveRect[]? moved,
                                           uint movedCapacity, out uint movedCount);
    // Collects and commits landed readbacks, drawing nothing. S_FALSE while one
    // is in flight; E_NOT_VALID_STATE without ContextFlags.DeferredAnalysisCommit.
    [PreserveSig] int PollReadbacks();
}
