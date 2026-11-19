namespace ConkerLauncher.Services;

/// <summary>
/// The game's save folder, where <see cref="UserFiles"/> keeps them (SPEC 9.1): saves,
/// <c>settings.ini</c>, <c>logs\</c> and <c>Photos\</c> (or <c>photo_folder</c> from settings.ini).
/// Play passes it to the game as <c>--save-dir</c>.
/// </summary>
public static class SaveFolder
{
    /// <summary>Set once at start-up.</summary>
    public static UserFiles? Files { get; set; }

    public static string Root => Override ?? Files?.Active ?? UserFiles.AppDataPath;

    /// <summary>
    /// <c>CONKER_LAUNCHER_SAVE_DIR</c> (development only): another save folder, so trying the
    /// Settings tab never touches the real settings.ini. Nothing is moved while it is set.
    /// </summary>
    public static string? Override =>
        Environment.GetEnvironmentVariable("CONKER_LAUNCHER_SAVE_DIR") is { Length: > 0 } dir ? Path.GetFullPath(dir) : null;

    public static string Logs => Path.Combine(Root, "logs");

    public static string SettingsIni => Path.Combine(Root, "settings.ini");

    public static string Photos => ReadSetting("photo_folder") is { Length: > 0 } folder ? Unquoted(folder) : Path.Combine(Root, "Photos");

    /// <summary>A path without the quotes the game allows around it (<c>normalize</c> in <c>src/settings.c</c>).</summary>
    private static string Unquoted(string path) =>
        path.Length >= 2 && path[0] == '"' && path[^1] == '"' ? path[1..^1] : path;

    /// <summary>
    /// A <c>key = value</c> from settings.ini (the keys are unique across sections), read as the
    /// game reads it (<see cref="SettingsFile.Read"/>: UTF-8, else the PC's own code page).
    /// </summary>
    public static string? ReadSetting(string key)
    {
        try
        {
            return SettingsFile.Read(SettingsIni).TryGetValue(key, out string? value) ? value : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }

        return null;
    }
}
