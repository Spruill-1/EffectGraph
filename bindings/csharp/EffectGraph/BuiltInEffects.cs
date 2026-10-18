// ShaderLab's built-in effects for .NET programs. C++ programs include a header
// per effect; here each is an .effectblob embedded in this assembly, registered
// by name before the first context uses it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

namespace EffectGraph;

public static class BuiltInEffects
{
    static readonly Assembly sAssembly = typeof(BuiltInEffects).Assembly;
    const string cSuffix = ".effectblob";

    // The file stem the generator gives an effect: its words, lowercased,
    // joined by '_' ("ICtCp Tone Map (HDR -> SDR)" -> ictcp_tone_map_hdr_sdr).
    public static string FileStem(string name)
    {
        var words = new List<string>();
        var word = new StringBuilder();
        foreach (char ch in name)
        {
            if (ch is (>= 'A' and <= 'Z') or (>= 'a' and <= 'z') or (>= '0' and <= '9'))
            {
                word.Append(char.ToLowerInvariant(ch));
                continue;
            }
            if (word.Length > 0) { words.Add(word.ToString()); word.Clear(); }
        }
        if (word.Length > 0) words.Add(word.ToString());
        return string.Join('_', words);
    }

    // The stems of every built-in this assembly carries.
    public static IReadOnlyList<string> Available { get; } =
        sAssembly.GetManifestResourceNames().Where(n => n.EndsWith(cSuffix, StringComparison.Ordinal))
                 .Select(n => n[..^cSuffix.Length]).OrderBy(n => n, StringComparer.Ordinal).ToArray();

    // Registers the built-in with this name ("Luminance Statistics"), so
    // CreateEffect can create it. Process-wide; registering twice is harmless.
    public static void Register(string name)
    {
        string resource = FileStem(name) + cSuffix;
        using Stream? stream = sAssembly.GetManifestResourceStream(resource)
            ?? throw new ArgumentException($"'{name}' is not a ShaderLab built-in effect", nameof(name));
        using var bytes = new MemoryStream();
        stream.CopyTo(bytes);
        EffectGraphLibrary.RegisterEffect(bytes.ToArray());
    }

    public static void RegisterAll()
    {
        foreach (string stem in Available)
        {
            using Stream stream = sAssembly.GetManifestResourceStream(stem + cSuffix)!;
            using var bytes = new MemoryStream();
            stream.CopyTo(bytes);
            EffectGraphLibrary.RegisterEffect(bytes.ToArray());
        }
    }
}
