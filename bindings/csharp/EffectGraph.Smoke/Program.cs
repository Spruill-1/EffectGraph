// The EffectGraph packages' test: a minimal C# program, using only what the
// packages give it, that makes a D3D11 device, gives a graph an HDR texture and
// reads statistics back, on the CPU and from the values buffer. Exit code 0
// when every check holds.
using System;
using System.IO;
using System.Runtime.InteropServices;
using EffectGraph;

int failures = 0;
void Check(string name, bool ok)
{
    Console.WriteLine($"  [{(ok ? "PASS" : "FAIL")}] {name}");
    if (!ok) failures++;
}

// A WARP device with BGRA support, as Direct2D needs.
const int D3D_DRIVER_TYPE_WARP = 5;
const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
Marshal.ThrowExceptionForHR(D3D11.D3D11CreateDevice(IntPtr.Zero, D3D_DRIVER_TYPE_WARP, IntPtr.Zero,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, IntPtr.Zero, 0, 7, out IntPtr devicePtr, out _, out IntPtr immediate));
var device = (D3D11.ID3D11Device)Marshal.GetObjectForIUnknown(devicePtr);

// 64 x 32 FP16 texels at 3.0 scRGB (240 nits), alpha 1.
const int width = 64, height = 32;
var texels = new ushort[width * height * 4];
for (int i = 0; i < texels.Length; i += 4)
{
    texels[i] = texels[i + 1] = texels[i + 2] = BitConverter.HalfToUInt16Bits((Half)3.0f);
    texels[i + 3] = BitConverter.HalfToUInt16Bits((Half)1.0f);
}
IntPtr texture;
unsafe
{
    fixed (ushort* data = texels)
    {
        var desc = new D3D11.Texture2DDesc
        {
            Width = width, Height = height, MipLevels = 1, ArraySize = 1,
            Format = 10 /* DXGI_FORMAT_R16G16B16A16_FLOAT */, SampleCount = 1,
            Usage = 0, BindFlags = 0x8 /* D3D11_BIND_SHADER_RESOURCE */,
        };
        var init = new D3D11.SubresourceData { SysMem = (IntPtr)data, SysMemPitch = width * 8 };
        texture = device.CreateTexture2D(desc, init);
    }
}

Console.WriteLine("EffectGraph from C#");
Check("Builtins_Embedded", BuiltInEffects.Available.Contains("luminance_statistics"));
// Under --expect-v1 (a 1.0.x DLL; see below) this package's blobs carry a
// newer shader contract, which the old DLL refuses; Pack.ps1 puts the 1.0
// package's own blob beside the program for the rest of the run.
if (args.Contains("--expect-v1"))
{
    try
    {
        BuiltInEffects.Register("Luminance Statistics");
        Check("V1_NewerContractRefused", false);
    }
    catch (COMException e)
    {
        Check("V1_NewerContractRefused", e.HResult == HResults.IncompatibleEffect);
    }
    EffectGraphLibrary.RegisterEffect(File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory,
                                                                     "luminance_statistics.effectblob")));
}
else
{
    BuiltInEffects.Register("Luminance Statistics");
}

var context = EffectGraphLibrary.CreateContext(devicePtr);
var input = context.CreateTextureInput(texture, InputEncoding.ScRgbLinear);
var stats = context.CreateEffect("Luminance Statistics");
stats.SetInput(0, input);
Check("Node_Named", stats.GetName() == "Luminance Statistics");

float max = stats.GetValues().GetFloat("Max");
Console.WriteLine($"  [info] Max {max:F3} nits");
Check("Values_Max", Math.Abs(max - 240.0f) < 0.05f);

var buffer = stats.GetValuesBufferOrNull();
Check("Buffer_Created", buffer != null);
if (buffer != null)
{
    var info = buffer.GetFieldInfo(buffer.GetFieldIndex("Max"));
    buffer.GetView(out IntPtr view);
    Check("Buffer_View", view != IntPtr.Zero);
    if (view != IntPtr.Zero) Marshal.Release(view);
    Check("Buffer_ReadValues", Math.Abs(buffer.ReadValues().GetFloat("Max") - 240.0f) < 0.05f);
    buffer.SetFloats(buffer.GetFieldIndex("Max"), new[] { 400.0f }, 1);
    Check("Buffer_Write", Math.Abs(stats.GetValues().GetFloat("Max") - 400.0f) < 0.05f);
    Console.WriteLine($"  [info] Max is field {buffer.GetFieldIndex("Max")} at element {info.BufferElement}");
}

// A built-in this program did not register says so.
try
{
    context.CreateEffect("Nit Map");
    Check("NotRegistered_Refused", false);
}
catch (COMException e)
{
    Check("NotRegistered_Refused", e.HResult == HResults.EffectNotIncluded);
}

// 1.1: the dirty-rect interfaces, reached by QueryInterface. Run with
// --expect-v1 against a 1.0.x EffectGraph.dll (Pack.ps1 does), where every
// object answers each v2 IID with E_NOINTERFACE, which the cast helpers
// surface as InvalidCastException, and the 1.0 calls above still work.
bool expectV1 = args.Contains("--expect-v1");
Console.WriteLine(expectV1 ? "  [info] expecting a 1.0 DLL: no v2 interfaces" : "  [info] expecting the 1.1 interfaces");
static bool CastFails(Action cast)
{
    try { cast(); return false; }
    catch (InvalidCastException) { return true; }
}
if (expectV1)
{
    Check("V1_ContextIsNotContext2", context is not IEffectGraphContext2 && CastFails(() => context.AsContext2()));
    Check("V1_EffectIsNotNode2", stats is not IEffectGraphNode2 && CastFails(() => stats.AsNode2()));
    Check("V1_EffectIsNotInput2", stats is not IEffectGraphInput2 && CastFails(() => stats.AsInput2()));
    Check("V1_InputIsNotNode2", input is not IEffectGraphNode2 && CastFails(() => input.AsNode2()));
    Check("V1_InputIsNotInput2", input is not IEffectGraphInput2 && CastFails(() => input.AsInput2()));
    Check("V1_InputIsNotContext2", input is not IEffectGraphContext2);
    // The 1.0 calls still work after the failed casts.
    var again = context.CreateEffect("Luminance Statistics");
    again.SetInput(0, input);
    Check("V1_StillWorks", Math.Abs(again.GetValues().GetFloat("Max") - 240.0f) < 0.05f);
    Marshal.ReleaseComObject(again);
}
else
{
    Check("V2_Context", context is IEffectGraphContext2);
    Check("V2_InputHasBoth", input is IEffectGraphInput2 && input is IEffectGraphNode2);
    Check("V2_EffectIsNode2", stats is IEffectGraphNode2);
    Check("V2_EffectIsNotInput2", stats is not IEffectGraphInput2 && CastFails(() => stats.AsInput2()));
    var input2 = input.AsInput2();
    Check("V2_InvalidateRects", input2.InvalidateRects(new[] { new Rect(0, 0, 8, 4) }, 1) == 0);
    // Whole: no RealizeToIncremental has drawn the input yet.
    var invalid = new Rect[4];
    Check("V2_GetInvalidRects", input.AsNode2().GetInvalidRects(invalid, 4, out uint invalidCount, out int whole) == 0 &&
                                whole != 0 && invalidCount == 0);
    // An empty move changes nothing (S_FALSE); a real one is taken.
    Check("V2_MoveRectsEmpty", input2.MoveRects(new MoveRect[1], 1) == 1);
    Check("V2_MoveRects", input2.MoveRects(new[] { new MoveRect { SourceX = 0, SourceY = 0, Destination = new Rect(4, 0, 8, 4) } }, 1) == 0);
    // Without ContextFlags.DeferredAnalysisCommit there is nothing to poll.
    const int E_NOT_VALID_STATE = unchecked((int)0x8007139F);
    Check("V2_PollReadbacksNeedsFlag", context.AsContext2().PollReadbacks() == E_NOT_VALID_STATE);
    Check("V2_TryGetValuesNeedsFlag", stats.AsNode2().TryGetValues(out var landed) == E_NOT_VALID_STATE && landed == null);
}

// Every context released (node objects hold theirs), then the DLL's threads.
Marshal.ReleaseComObject(stats);
Marshal.ReleaseComObject(input);
if (buffer != null) Marshal.ReleaseComObject(buffer);
Marshal.ReleaseComObject(context);
GC.Collect();
GC.WaitForPendingFinalizers();
EffectGraphLibrary.Shutdown();
Marshal.Release(texture);
Marshal.Release(immediate);
Marshal.ReleaseComObject(device);
Console.WriteLine(failures == 0 ? "ALL PASSED" : $"{failures} FAILED");
return failures == 0 ? 0 : 1;

static class D3D11
{
    [DllImport("d3d11.dll", ExactSpelling = true)]
    public static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software, uint flags,
        IntPtr featureLevels, uint featureLevelCount, uint sdkVersion, out IntPtr device, out int featureLevel,
        out IntPtr immediateContext);

    [StructLayout(LayoutKind.Sequential)]
    public struct Texture2DDesc
    {
        public uint Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality, Usage, BindFlags,
                    CPUAccessFlags, MiscFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SubresourceData
    {
        public IntPtr SysMem;
        public uint SysMemPitch, SysMemSlicePitch;
    }

    // The first methods of ID3D11Device, up to the one used here.
    [ComImport, Guid("db6f6ddb-ac77-4e88-8253-819df9bbf140"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface ID3D11Device
    {
        void CreateBuffer();
        void CreateTexture1D();
        IntPtr CreateTexture2D(in Texture2DDesc desc, in SubresourceData initialData);
    }
}
