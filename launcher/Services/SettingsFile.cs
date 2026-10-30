using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// Reads and changes <c>settings.ini</c> in the save folder. Unlike the game's F2 menu, which
/// writes the whole file again, a change here only replaces the option's own line, so the
/// player's comments and lines the game doesn't know stay (SPEC 9.3). Latin-1 reads and writes
/// every byte back as it was (paths may be in the Windows code page; our values are ASCII).
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

    private static readonly Encoding FileEncoding = Encoding.Latin1;

    /// <summary>Every <c>key = value</c> in the file (a later line wins, as in the game); empty when there is no file.</summary>
    public static Dictionary<string, string> Read(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(path))
        {
            return values;
        }

        foreach (string line in File.ReadAllLines(path, FileEncoding))
        {
            if (KeyOf(line) is { } key)
            {
                values[key] = line[(line.IndexOf('=') + 1)..].Trim();
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
        List<string> lines = File.Exists(path)
            ? [.. File.ReadAllLines(path, FileEncoding)]
            : NewFile(schema);

        foreach ((string key, string value) in changes)
        {
            string text = $"{key} = {value}";
            int at = lines.FindIndex(l => key.Equals(KeyOf(l), StringComparison.OrdinalIgnoreCase));
            if (at >= 0)
            {
                lines[at] = text;
                continue;
            }

            string section = schema.ByKey.TryGetValue(key, out SettingOption? option) ? option.Section : "";
            int header = lines.FindIndex(l => l.Trim().Equals($"[{section}]", StringComparison.OrdinalIgnoreCase));
            if (header < 0)
            {
                lines.AddRange(["", $"[{section}]", text]);
                continue;
            }

            // After the section's last line that isn't blank, before the next section.
            int end = lines.FindIndex(header + 1, l => l.TrimStart().StartsWith('['));
            int last = end < 0 ? lines.Count - 1 : end - 1;
            while (last > header && lines[last].Trim().Length == 0)
            {
                last--;
            }

            lines.Insert(last + 1, text);
        }

        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + ".launcher.tmp";
        File.WriteAllLines(temp, lines, FileEncoding);
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

            lines.Add($"{o.Key} = {o.Default}");
        }

        return lines;
    }
}
