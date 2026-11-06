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

    private static string? Read(string key)
    {
        try
        {
            return File.Exists(Path) ? SettingsFile.Read(Path).GetValueOrDefault(key) is { Length: > 0 } v ? v : null : null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    private static void Write(string key, string value)
    {
        try
        {
            Dictionary<string, string> values = File.Exists(Path) ? SettingsFile.Read(Path) : [];
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
