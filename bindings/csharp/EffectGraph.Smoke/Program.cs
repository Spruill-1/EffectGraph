// The EffectGraph packages' test: a minimal C# program, using only what the
// packages give it, that makes a D3D11 device, gives a graph an HDR texture and
// reads statistics back, on the CPU and from the values buffer. Exit code 0
// when every check holds.
using System;
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
BuiltInEffects.Register("Luminance Statistics");

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
