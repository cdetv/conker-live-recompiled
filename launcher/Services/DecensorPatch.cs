namespace ConkerLauncher.Services;

/// <summary>
/// The decensor patch by nastys, Vaani and Theconker64 (SPEC 9.3, 10.1): never shipped with the
/// launcher (their repo has no license), added by the player from a .7z/.zip or downloaded from
/// their latest release, and unpacked into <c>decensor\</c>, where <c>conker.exe</c> looks for
/// <c>decensor\dvddata\</c>.
/// </summary>
/// <remarks>
/// <c>decensor\version.txt</c> holds the release tag it came from (from the download, or the
/// archive's file name such as <c>...-0.37.7z</c>), so Update can tell when a newer one is out.
/// conker.exe only scans <c>dvddata\</c>, so the extra file does no harm.
/// </remarks>
public sealed class DecensorPatch(GameFolder folder)
{
    public const string Repo = "nastys/conker-live-and-reloaded-decensor";

    public static readonly string[] Extensions = [".7z", ".zip"];

    private string Dir => Path.Combine(folder.Root, "decensor");

    private string Staging => Path.Combine(folder.Root, "decensor.import");

    private string VersionFile => Path.Combine(Dir, "version.txt");

    public bool IsInstalled => Directory.Exists(Path.Combine(Dir, "dvddata"));

    /// <summary>The installed release's tag, or null when unknown.</summary>
    public string? InstalledVersion
    {
        get
        {
            try
            {
                return File.Exists(VersionFile) ? File.ReadAllText(VersionFile).Trim() is { Length: > 0 } v ? v : null : null;
            }
            catch (IOException)
            {
                return null;
            }
        }
    }

    /// <summary>True when <paramref name="latest"/> is a newer release than the installed one.</summary>
    public bool IsOlderThan(string latest) =>
        InstalledVersion is { } installed && Version.TryParse(installed, out Version? have)
        && Version.TryParse(latest, out Version? newest) && newest > have;

    public void CleanLeftovers() => ImportTools.RemoveFolder(Staging);

    /// <summary>Unpacks the patch from <paramref name="archivePath"/> into <c>decensor\</c>.</summary>
    public void Install(string archivePath, string? version, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        CleanLeftovers();
        try
        {
            progress.Report(new JobProgress("Opening the patch", null));
            using var archive = ImportTools.OpenArchive(archivePath);
            List<(string Key, long Size)> files = ImportTools.ListFiles(archive);

            // The patch is laid out like the disc: everything from its dvddata folder on.
            int at = files.Select(f => DvddataAt(f.Key)).Where(i => i >= 0).DefaultIfEmpty(-1).Min();
            string? prefix = at < 0 ? null : files.Select(f => f.Key).First(k => DvddataAt(k) == at)[..at];
            if (prefix is null)
            {
                throw new ImportException($"{Path.GetFileName(archivePath)} isn't the decensor patch (it has no dvddata folder).");
            }

            List<(string Key, long Size)> wanted = [.. files.Where(f => f.Key.StartsWith(prefix + "dvddata/", StringComparison.OrdinalIgnoreCase))];
            long total = wanted.Sum(f => f.Size);
            ImportTools.RequireSpace(folder.Root, total);
            ImportTools.Extract(archive, Staging,
                key => key.StartsWith(prefix + "dvddata/", StringComparison.OrdinalIgnoreCase) ? key[prefix.Length..] : null,
                new ByteProgress(progress, "Unpacking the decensor patch", total), cancel);
            File.WriteAllText(Path.Combine(Staging, "version.txt"), (version ?? VersionFromName(archivePath) ?? "") + Environment.NewLine);
            progress.Report(new JobProgress("Finishing", null));
            ImportTools.Swap(Staging, Dir);
        }
        finally
        {
            CleanLeftovers();
        }
    }

    /// <summary>Downloads the latest release to a file next to <c>decensor\</c> and installs it.</summary>
    public async Task DownloadAndInstallAsync(ReleaseInfo release, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        if (release.AssetUrl is null)
        {
            throw new ImportException($"The latest decensor release ({release.Tag}) has no .7z or .zip to download.");
        }

        ImportTools.RequireSpace(folder.Root, release.AssetSize * 3);
        string download = Path.Combine(folder.Root, "decensor.download");
        try
        {
            await GitHubReleases.DownloadAsync(release.AssetUrl, download,
                new ByteProgress(progress, $"Downloading the decensor patch {release.Tag}", release.AssetSize), cancel);
            await Task.Run(() => Install(download, release.Tag, progress, cancel), cancel);
        }
        finally
        {
            File.Delete(download);
        }
    }

    private static int DvddataAt(string key)
    {
        if (key.StartsWith("dvddata/", StringComparison.OrdinalIgnoreCase))
        {
            return 0;
        }

        int i = key.IndexOf("/dvddata/", StringComparison.OrdinalIgnoreCase);
        return i < 0 ? -1 : i + 1;
    }

    // "conker-live-and-reloaded-decensor-0.37.7z" -> "0.37"
    private static string? VersionFromName(string path)
    {
        string name = Path.GetFileNameWithoutExtension(path);
        int dash = name.LastIndexOf('-');
        string tail = dash >= 0 ? name[(dash + 1)..] : name;
        return Version.TryParse(tail, out _) ? tail : null;
    }
}
