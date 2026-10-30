namespace ConkerLauncher.Services;

/// <summary>
/// The game's save folder, <c>%LOCALAPPDATA%\conker-lr-recomp</c> (SPEC 5): saves,
/// <c>settings.ini</c>, <c>logs\</c> and <c>Photos\</c> (or <c>photo_folder</c> from settings.ini).
/// </summary>
public static class SaveFolder
{
    public static string Root => Override ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "conker-lr-recomp");

    /// <summary>
    /// <c>CONKER_LAUNCHER_SAVE_DIR</c> (development only): another save folder, so trying the
    /// Settings tab never touches the real settings.ini; Play then passes it as <c>--save-dir</c>.
    /// </summary>
    public static string? Override =>
        Environment.GetEnvironmentVariable("CONKER_LAUNCHER_SAVE_DIR") is { Length: > 0 } dir ? Path.GetFullPath(dir) : null;

    public static string Logs => Path.Combine(Root, "logs");

    public static string SettingsIni => Path.Combine(Root, "settings.ini");

    public static string Photos => ReadSetting("photo_folder") is { Length: > 0 } folder ? folder : Path.Combine(Root, "Photos");

    /// <summary>A <c>key = value</c> from settings.ini (the keys are unique across sections).</summary>
    public static string? ReadSetting(string key)
    {
        try
        {
            if (!File.Exists(SettingsIni))
            {
                return null;
            }

            foreach (string line in File.ReadLines(SettingsIni))
            {
                int eq = line.IndexOf('=');
                if (eq > 0 && !line.TrimStart().StartsWith(';') && line[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                {
                    return line[(eq + 1)..].Trim();
                }
            }
        }
        catch (IOException)
        {
        }

        return null;
    }
}
