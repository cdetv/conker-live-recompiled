using System.Globalization;
using System.Text;
using Microsoft.Win32;

namespace ConkerLauncher.Services;

/// <summary>
/// Reads and changes <c>settings.ini</c> in the save folder. Unlike the game's F2 menu, which
/// writes the whole file again, a change here only replaces the option's own line, so the
/// player's comments and lines the game doesn't know stay (SPEC 9.3). The file is UTF-8, as the
/// game writes it (to-do 262: folder names with any letters); a line an editor saved in the PC's
/// own code page (not valid UTF-8) is read in that code page, as the game reads it
/// (<c>src/legacy_cp.h</c>). Every line not changed here is written back byte for byte, so
/// nothing the launcher doesn't touch can change; changed and new lines are UTF-8.
/// </summary>
public static class SettingsFile
{
    private const string Header =
        "; Conker: Live & Reloaded (PC recompilation) settings.\n" +
        "; Change them in the game with F2, or here with any text editor\n" +
        "; (changes made here apply the next time the game starts). Lines\n" +
        "; starting with ; are comments; F2 writes the whole file again,\n" +
        "; without lines it does not know. Delete this file to get the\n" +
        "; defaults back.";

    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    private static readonly UTF8Encoding Utf8Strict = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    private static readonly byte[] Bom = [0xEF, 0xBB, 0xBF];

    /// <summary>
    /// The PC's own ANSI code page ("language for non-Unicode programs"): what GetACP() returns
    /// for a program without a UTF-8 manifest, from the registry value it reads, as the game's
    /// <c>conker_legacy_cp</c>. Latin-1 when that code page isn't there.
    /// </summary>
    private static readonly Lazy<Encoding> Legacy = new(() =>
    {
        try
        {
            if (Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Nls\CodePage", "ACP", null) is string acp
                && int.TryParse(acp, NumberStyles.None, CultureInfo.InvariantCulture, out int cp)
                && CodePagesEncodingProvider.Instance.GetEncoding(cp) is { } encoding)
            {
                return encoding;
            }
        }
        catch (Exception e) when (e is System.Security.SecurityException or IOException or UnauthorizedAccessException)
        {
        }

        return Encoding.Latin1;
    });

    /// <summary>One line: its bytes as they were in the file (null for a line made here) and its text.</summary>
    private sealed record Line(byte[]? Raw, string Text);

    /// <summary>A line's bytes as text: UTF-8, else the PC's own code page.</summary>
    private static string Decode(byte[] raw)
    {
        try
        {
            return Utf8Strict.GetString(raw);
        }
        catch (DecoderFallbackException)
        {
            return Legacy.Value.GetString(raw);
        }
    }

    /// <summary>The file's lines (without CR / LF); <paramref name="bom"/>: it started with a UTF-8 byte order mark.</summary>
    private static List<Line> Load(string path, out bool bom)
    {
        byte[] all = File.ReadAllBytes(path);
        bom = all.AsSpan().StartsWith(Bom);
        var lines = new List<Line>();
        int start = bom ? Bom.Length : 0;
        while (start < all.Length)
        {
            int end = Array.IndexOf(all, (byte)'\n', start);
            int stop = end < 0 ? all.Length : end;
            int length = stop - start;
            if (length > 0 && all[stop - 1] == '\r')
            {
                length--;
            }

            byte[] raw = all.AsSpan(start, length).ToArray();
            lines.Add(new Line(raw, Decode(raw)));
            start = stop + 1;
        }

        return lines;
    }

    /// <summary>Every <c>key = value</c> in the file (a later line wins, as in the game); empty when there is no file.</summary>
    public static Dictionary<string, string> Read(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(path))
        {
            return values;
        }

        foreach (Line line in Load(path, out _))
        {
            if (KeyOf(line.Text) is { } key)
            {
                values[key] = line.Text[(line.Text.IndexOf('=') + 1)..].Trim();
            }
        }

        return values;
    }

    /// <summary>
    /// Sets each key's line to its new value: in place where the file has it, else at the end
    /// of its section (or a new section at the end). A missing file is written as the game
    /// writes a new one, every option with its comment, at its default but for these values.
    /// </summary>
    public static void Write(string path, SettingsSchema schema, IReadOnlyDictionary<string, string> changes)
    {
        bool bom = false;
        List<Line> lines = File.Exists(path)
            ? Load(path, out bom)
            : [.. NewFile(schema).Select(t => new Line(null, t))];

        foreach ((string key, string value) in changes)
        {
            var text = new Line(null, $"{key} = {value}");
            int at = lines.FindIndex(l => key.Equals(KeyOf(l.Text), StringComparison.OrdinalIgnoreCase));
            if (at >= 0)
            {
                lines[at] = text;
                continue;
            }

            string section = schema.ByKey.TryGetValue(key, out SettingOption? option) ? option.Section : "";
            int header = lines.FindIndex(l => l.Text.Trim().Equals($"[{section}]", StringComparison.OrdinalIgnoreCase));
            if (header < 0)
            {
                lines.AddRange([new Line(null, ""), new Line(null, $"[{section}]"), text]);
                continue;
            }

            // After the section's last line that isn't blank, before the next section.
            int end = lines.FindIndex(header + 1, l => l.Text.TrimStart().StartsWith('['));
            int last = end < 0 ? lines.Count - 1 : end - 1;
            while (last > header && lines[last].Text.Trim().Length == 0)
            {
                last--;
            }

            lines.Insert(last + 1, text);
        }

        Save(path, lines, bom);
    }

    // ---- [texture_packs] (to-do 288 part 2d) -------------------------------------------------

    private const string PacksHeader = "[texture_packs]";

    /// <summary>The section's comment as the game writes it (<c>write_packs</c> in <c>src/settings.c</c>).</summary>
    private static readonly string[] PacksComment =
    [
        "; Texture packs: folders of PNG textures in textures\\packs next to",
        "; conker.exe, one line each, the top one first (it wins where two packs",
        "; change the same texture): on = used, off = listed but not used. A new",
        "; folder starts off. F2 > Extras > Texture Packs changes them in the",
        "; game, at once.",
    ];

    private static bool IsPacksHeader(string line) => line.Trim().Equals(PacksHeader, StringComparison.OrdinalIgnoreCase);

    private static bool IsHeader(string line) => line.TrimStart().StartsWith('[');

    /// <summary>
    /// settings.ini's <c>[texture_packs]</c>: one <c>on = name</c> / <c>off = name</c> line a pack, in
    /// order, the top one first. Read as the game reads it (<c>pack_line</c>): other keys, empty names,
    /// names of 128 bytes or more, a name given twice (the first counts) and lines past 64 are left out.
    /// Empty when there is no file or no section.
    /// </summary>
    public static List<(string Name, bool On)> ReadPacks(string path)
    {
        var packs = new List<(string Name, bool On)>();
        if (!File.Exists(path))
        {
            return packs;
        }

        bool inside = false;
        foreach (Line line in Load(path, out _))
        {
            if (IsHeader(line.Text))
            {
                inside = IsPacksHeader(line.Text);
                continue;
            }

            if (!inside || KeyOf(line.Text) is not { } key)
            {
                continue;
            }

            bool on = key.Equals("on", StringComparison.OrdinalIgnoreCase);
            string name = line.Text[(line.Text.IndexOf('=') + 1)..].Trim();
            if ((on || key.Equals("off", StringComparison.OrdinalIgnoreCase)) && name.Length > 0
                && Utf8.GetByteCount(name) < 128 && packs.Count < 64
                && !packs.Exists(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            {
                packs.Add((name, on));
            }
        }

        return packs;
    }

    /// <summary>
    /// Replaces the <c>[texture_packs]</c> section's pack lines with <paramref name="packs"/> (its
    /// heading and comments stay; every other line of the file stays byte for byte). Without the
    /// section, it goes before <c>[binds]</c> (or at the end) with the game's comment. A missing file
    /// is written as the game writes a new one (<paramref name="schema"/>; the Settings tab's way),
    /// so the game's first start on this PC still picks its preset: without a schema, nothing is
    /// written and this throws <see cref="InvalidOperationException"/>.
    /// </summary>
    public static void WritePacks(string path, SettingsSchema? schema, IReadOnlyList<(string Name, bool On)> packs)
    {
        bool bom = false;
        List<Line> lines;
        if (File.Exists(path))
        {
            lines = Load(path, out bom);
        }
        else if (schema is not null)
        {
            lines = [.. NewFile(schema).Select(t => new Line(null, t))];
        }
        else
        {
            throw new InvalidOperationException("there is no settings.ini yet");
        }

        IEnumerable<Line> packLines = packs.Select(p => new Line(null, $"{(p.On ? "on" : "off")} = {p.Name}"));
        int header = lines.FindIndex(l => IsPacksHeader(l.Text));
        if (header >= 0)
        {
            int end = lines.FindIndex(header + 1, l => IsHeader(l.Text));
            end = end < 0 ? lines.Count : end;
            for (int i = end - 1; i > header; i--)
            {
                if (KeyOf(lines[i].Text) is not null)
                {
                    lines.RemoveAt(i);
                    end--;
                }
            }

            // After the heading and its comments, before the blank line ahead of the next section.
            int at = end;
            while (at > header + 1 && lines[at - 1].Text.Trim().Length == 0)
            {
                at--;
            }

            lines.InsertRange(at, packLines);
        }
        else
        {
            List<Line> section = [new Line(null, ""), new Line(null, PacksHeader),
                                  .. PacksComment.Select(c => new Line(null, c)), .. packLines];
            int binds = lines.FindIndex(l => l.Text.Trim().Equals("[binds]", StringComparison.OrdinalIgnoreCase));
            if (binds < 0)
            {
                lines.AddRange(section);
            }
            else
            {
                // The blank line before [binds] stays the one before it.
                int at = binds;
                while (at > 0 && lines[at - 1].Text.Trim().Length == 0)
                {
                    at--;
                }

                lines.InsertRange(at, section);
            }
        }

        Save(path, lines, bom);
    }

    private static void Save(string path, List<Line> lines, bool bom)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + ".launcher.tmp";
        using (var file = new FileStream(temp, FileMode.Create, FileAccess.Write))
        {
            if (bom)
            {
                file.Write(Bom);
            }

            foreach (Line line in lines)
            {
                file.Write(line.Raw ?? Utf8.GetBytes(line.Text));
                file.Write("\r\n"u8);
            }
        }

        File.Move(temp, path, overwrite: true);
    }

    /// <summary>The key of a <c>key = value</c> line; null for comments, headings and blank lines.</summary>
    private static string? KeyOf(string line)
    {
        string t = line.Trim();
        int eq = t.IndexOf('=');
        return eq > 0 && t[0] != ';' && t[0] != '#' && t[0] != '[' ? t[..eq].Trim() : null;
    }

    /// <summary>The file as the game writes a new one (<c>write_file</c> in <c>src/settings.c</c>).</summary>
    private static List<string> NewFile(SettingsSchema schema)
    {
        List<string> lines = [.. Header.Split('\n')];
        string section = "";
        foreach (SettingOption o in schema.Options)
        {
            if (o.Section != section)
            {
                section = o.Section;
                lines.AddRange(["", $"[{section}]"]);
            }
            else if (o.Comment is not null)
            {
                lines.Add("");
            }

            if (o.Comment is not null)
            {
                lines.AddRange(o.Comment.Split('\n').Select(l => "; " + l));
            }

            lines.Add($"{o.Key} = {schema.NewFileValue(o)}");
        }

        return lines;
    }
}
