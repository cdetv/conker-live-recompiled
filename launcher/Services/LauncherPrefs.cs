using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// The launcher's own choices (SPEC 9.3), in <c>launcher.ini</c> next to settings.ini in the
/// save folder: not in settings.ini, because the game's F2 menu writes that file again
/// without the lines it doesn't know. Updates never touch the save folder.
/// </summary>
/// <remarks>
/// <c>auto_update</c> (0/1, default 0): install a new release at start without asking.
/// <c>skip_update</c> (a version, e.g. 0.2.1): "Don't remind me" for that release; the
/// update window shows again once a newer one is out.
/// <c>recent_folders</c>: the last 5 folders a file was chosen from in the file browser
/// (to-do 122), newest first, separated by <c>|</c> (never in a Windows path).
/// The file is UTF-8 (folder names), unlike settings.ini (Latin-1, the game's).
/// </remarks>
public static class LauncherPrefs
{
    private const string Header =
        "; Conker: Live & Recompiled launcher choices (the launcher writes this file).";

    private static string Path => System.IO.Path.Combine(SaveFolder.Root, "launcher.ini");

    public static bool AutoUpdate
    {
        get => Read("auto_update") == "1";
        set => Write("auto_update", value ? "1" : "0");
    }

    /// <summary>The release "Don't remind me" was ticked for, or null.</summary>
    public static Version? SkippedUpdate
    {
        get => Read("skip_update") is { } v ? Updater.TagVersion(v) : null;
        set => Write("skip_update", value?.ToString() ?? "");
    }

    /// <summary>The file browser's recent folders, newest first.</summary>
    public static IReadOnlyList<string> RecentFolders =>
        Read("recent_folders")?.Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries) ?? [];

    /// <summary>A file was chosen from this folder: first in the recent folders (5 kept).</summary>
    public static void AddRecentFolder(string folder)
    {
        string trimmed = System.IO.Path.TrimEndingDirectorySeparator(folder);
        IEnumerable<string> rest = RecentFolders.Where(f =>
            !System.IO.Path.TrimEndingDirectorySeparator(f).Equals(trimmed, StringComparison.OrdinalIgnoreCase));
        Write("recent_folders", string.Join('|', rest.Prepend(folder).Take(MaxRecent)));
    }

    private const int MaxRecent = 5;

    private static string? Read(string key)
    {
        try
        {
            return ReadAll().GetValueOrDefault(key) is { Length: > 0 } v ? v : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    // UTF-8 (File.WriteAllText's default), so folder names keep their letters.
    private static Dictionary<string, string> ReadAll()
    {
        Dictionary<string, string> values = new(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(Path))
        {
            return values;
        }

        foreach (string line in File.ReadAllLines(Path, Encoding.UTF8))
        {
            string t = line.Trim();
            if (t.Length > 0 && t[0] is not ';' and not '[' && t.IndexOf('=', StringComparison.Ordinal) is > 0 and var eq)
            {
                values[t[..eq].Trim()] = t[(eq + 1)..].Trim();
            }
        }

        return values;
    }

    private static void Write(string key, string value)
    {
        try
        {
            Dictionary<string, string> values = ReadAll();
            values[key] = value;
            Directory.CreateDirectory(SaveFolder.Root);
            StringBuilder text = new(Header + "\r\n");
            foreach ((string k, string v) in values.OrderBy(p => p.Key, StringComparer.Ordinal))
            {
                text.Append(k).Append(" = ").Append(v).Append("\r\n");
            }

            File.WriteAllText(Path, text.ToString());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Asked again at the next start.
        }
    }
}
