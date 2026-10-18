// The C entry points of EffectGraph.dll, and the conveniences C# wants around
// the COM interfaces: strings, typed property values, error codes.
using System;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

[assembly: SupportedOSPlatform("windows")]

namespace EffectGraph;

public static class HResults
{
    public const int NodeRemoved = unchecked((int)0x80040E01);
    public const int WrongContext = unchecked((int)0x80040E02);
    public const int Cycle = unchecked((int)0x80040E03);
    public const int UnknownEffect = unchecked((int)0x80040E04);
    public const int CompilePending = unchecked((int)0x80040E05);
    public const int NodeFailed = unchecked((int)0x80040E06);
    public const int UnsupportedDevice = unchecked((int)0x80040E07);
    public const int UnsupportedFormat = unchecked((int)0x80040E08);
    public const int IncompatibleEffect = unchecked((int)0x80040E09);
    public const int EffectNotIncluded = unchecked((int)0x80040E0A);
}

public static class EffectGraphLibrary
{
    const string Dll = "EffectGraph";

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct ContextDesc
    {
        public ContextFlags Flags;
        public string? ShaderCacheDirectory;
    }

    [DllImport(Dll, ExactSpelling = true)]
    static extern int EffectGraphCreateContext(IntPtr device, in ContextDesc desc, in Guid riid, out IntPtr context);
    [DllImport(Dll, ExactSpelling = true)]
    static extern int EffectGraphRegisterEffect(byte[] data, uint size);
    [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Unicode)]
    static extern int EffectGraphLoadEffects(string directory);
    [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Unicode)]
    static extern int EffectGraphLoadPlugin(string path);
    [DllImport(Dll, ExactSpelling = true)]
    static extern void EffectGraphShutdown();

    // A context on `d3d11Device` (an ID3D11Device*), which must have been
    // created with D3D11_CREATE_DEVICE_BGRA_SUPPORT.
    public static IEffectGraphContext CreateContext(IntPtr d3d11Device, ContextFlags flags = ContextFlags.None,
                                                    string? shaderCacheDirectory = null)
    {
        var desc = new ContextDesc { Flags = flags, ShaderCacheDirectory = shaderCacheDirectory };
        Guid iid = typeof(IEffectGraphContext).GUID;
        Marshal.ThrowExceptionForHR(EffectGraphCreateContext(d3d11Device, desc, iid, out IntPtr context));
        try
        {
            return (IEffectGraphContext)Marshal.GetObjectForIUnknown(context);
        }
        finally
        {
            Marshal.Release(context);
        }
    }

    // Registers the effects in a blob written by EffectGraphCompile (or a
    // built-in's .effectblob). Returns S_OK, or S_FALSE when some were not
    // registered because newer versions are; throws for anything worse.
    public static int RegisterEffect(byte[] blob)
    {
        int hr = EffectGraphRegisterEffect(blob, (uint)blob.Length);
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        return hr;
    }

    public static int LoadEffects(string directory)
    {
        int hr = EffectGraphLoadEffects(directory);
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        return hr;
    }

    // Loads an effect plugin DLL (EffectGraphPlugin.h) by full path; it stays
    // loaded. Returns the plugin's result, or S_FALSE when it is already
    // loaded; throws when it does not load or is not a plugin.
    public static int LoadPlugin(string path)
    {
        int hr = EffectGraphLoadPlugin(path);
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        return hr;
    }

    // Once, after releasing every context.
    public static void Shutdown() => EffectGraphShutdown();
}

public static class EffectGraphExtensions
{
    public static string GetName(this IEffectGraphNode node)
    {
        var buffer = new char[node.GetNameLength() + 1];
        node.GetName(buffer, (uint)buffer.Length);
        return new string(buffer, 0, buffer.Length - 1);
    }

    public static string GetError(this IEffectGraphNode node)
    {
        var buffer = new char[node.GetErrorLength() + 1];
        node.GetError(buffer, (uint)buffer.Length);
        return new string(buffer, 0, buffer.Length - 1);
    }

    public static unsafe void SetFloat(this IEffectGraphNode node, string property, float value)
        => node.SetValueByName(property, PropertyType.Float, (IntPtr)(&value), sizeof(float));

    public static unsafe float GetFloat(this IEffectGraphNode node, string property)
    {
        float value = 0;
        node.GetValueByName(property, PropertyType.Float, (IntPtr)(&value), sizeof(float));
        return value;
    }

    public static float[] GetFloats(this IEffectGraphValues values, string field)
    {
        uint index = values.GetFieldIndex(field);
        if (index == uint.MaxValue) throw new ArgumentException($"No field '{field}'", nameof(field));
        var info = values.GetFieldInfo(index);
        var floats = new float[info.ComponentCount * info.ElementCount];
        values.GetFloats(index, floats, (uint)floats.Length);
        return floats;
    }

    // The node's values buffer, or null for a node with no analysis fields.
    public static IEffectGraphValuesBuffer? GetValuesBufferOrNull(this IEffectGraphNode node)
    {
        int hr = node.GetValuesBuffer(out var buffer);
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        return buffer;
    }

    public static IEffectGraphInput AsInput(this IEffectGraphNode node) => (IEffectGraphInput)node;
}
