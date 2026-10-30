namespace ConkerLauncher.Services;

/// <summary>
/// The release folder the launcher sits in (SPEC 9.1): <c>conker.exe</c> next to it, the
/// imported disc in <c>game\</c>, the decensor patch in <c>decensor\</c>.
/// </summary>
public sealed class GameFolder(string root)
{
    public string Root { get; } = root;

    public string GameExe => Path.Combine(Root, "conker.exe");

    public string GameDir => Path.Combine(Root, "game");

    public string DefaultXbe => Path.Combine(GameDir, "default.xbe");

    public bool GameFilesImported => File.Exists(DefaultXbe);

    /// <summary>
    /// The launcher's own folder. <c>CONKER_LAUNCHER_DIR</c> (development only) points it at
    /// another one, such as <c>build\Release</c> with its <c>game</c> junction.
    /// </summary>
    public static GameFolder FromEnvironment() =>
        new(Environment.GetEnvironmentVariable("CONKER_LAUNCHER_DIR") is { Length: > 0 } dir
            ? Path.GetFullPath(dir)
            : AppContext.BaseDirectory);
}
