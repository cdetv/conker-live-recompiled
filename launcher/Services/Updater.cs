using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;

namespace ConkerLauncher.Services;

/// <summary>
/// Updates the launcher's folder to the latest release of <see cref="Repo"/> (SPEC 9.3): the
/// release zip's files replace ours, everything else in the folder stays (<c>game\</c>,
/// <c>decensor\</c>, the user files in <c>userdata\</c>, anything the player added).
/// </summary>
/// <remarks>
/// The zip is downloaded to <c>update.download</c> and unpacked to <c>update.tmp\</c> next to
/// the files, so moving them in is a rename. Each file being replaced is first renamed to
/// <c>&lt;name&gt;.update-old</c>: Windows lets a running .exe be renamed but not deleted, which
/// is how the launcher replaces itself. A failure part way puts every renamed file back. The
/// names of the old files go into <c>update.cleanup</c>; the new launcher deletes them once
/// the old one has exited. <c>CONKER_LAUNCHER_UPDATE_URL</c> (development only) replaces
/// GitHub with any URL answering in its "latest release" format.
/// </remarks>
public sealed class Updater(GameFolder folder)
{
    public const string Repo = "cdetv/conker-live-recompiled";

    private const string OldSuffix = ".update-old";

    private static readonly string[] Extensions = [".zip"];

    // Never written by an update: the player's files (SPEC 9.1).
    private static readonly string[] Protected = ["game/", "decensor/", "userdata/", "settings.ini", UserFiles.MarkerName];

    private string Download => Path.Combine(folder.Root, "update.download");

    private string Staging => Path.Combine(folder.Root, "update.tmp");

    private string CleanupList => Path.Combine(folder.Root, "update.cleanup");

    /// <summary>The running launcher's version (major.minor.patch, as the VERSION file).</summary>
    public static Version Current { get; } = Assembly.GetExecutingAssembly().GetName().Version is { } v
        ? new Version(v.Major, v.Minor, Math.Max(v.Build, 0))
        : new Version(0, 0, 0);

    /// <summary>The latest release, from GitHub or the development override.</summary>
    public static Task<ReleaseInfo> GetLatestAsync(CancellationToken cancel) =>
        Environment.GetEnvironmentVariable("CONKER_LAUNCHER_UPDATE_URL") is { Length: > 0 } url
            ? GitHubReleases.GetLatestFromAsync(url, Extensions, cancel)
            : GitHubReleases.GetLatestAsync(Repo, Extensions, cancel);

    /// <summary>
    /// Every release, newest first, for the Version History window.
    /// <c>CONKER_LAUNCHER_HISTORY_URL</c> (development only) replaces GitHub with any URL
    /// answering in its "list releases" format.
    /// </summary>
    public static Task<List<ReleaseInfo>> GetHistoryAsync(CancellationToken cancel) =>
        Environment.GetEnvironmentVariable("CONKER_LAUNCHER_HISTORY_URL") is { Length: > 0 } url
            ? GitHubReleases.GetAllFromAsync(url, Extensions, cancel)
            : GitHubReleases.GetAllAsync(Repo, Extensions, cancel);

    /// <summary>The version a release tag names ("v0.2.0" or "0.2.0"), or null.</summary>
    public static Version? TagVersion(string tag) =>
        Version.TryParse(tag.TrimStart('v', 'V'), out Version? v) ? new Version(v.Major, v.Minor, Math.Max(v.Build, 0)) : null;

    /// <summary>True when <paramref name="release"/> is newer than this launcher and has a zip.</summary>
    public static bool IsNewer(ReleaseInfo release) =>
        release.AssetUrl is not null && TagVersion(release.Tag) is { } v && v > Current;

    /// <summary>Downloads <paramref name="release"/> and puts its files in place of ours.</summary>
    public async Task ApplyAsync(ReleaseInfo release, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        if (release.AssetUrl is null)
        {
            throw new ImportException($"Release {release.Tag} has no zip to download.");
        }

        ImportTools.RequireSpace(folder.Root, release.AssetSize * 4);
        CleanLeftovers();
        try
        {
            await GitHubReleases.DownloadAsync(release.AssetUrl, Download,
                new ByteProgress(progress, $"Downloading {release.Tag}", release.AssetSize), cancel);
            progress.Report(new JobProgress("Unpacking the update", null));
            List<string> files = await Task.Run(() => Unpack(cancel), cancel);
            progress.Report(new JobProgress("Replacing the program files", null));
            Install(files);
        }
        finally
        {
            CleanLeftovers();
        }
    }

    /// <summary>Starts the (new) launcher in this folder; the caller then closes.</summary>
    public void Restart()
    {
        var start = new ProcessStartInfo(Path.Combine(folder.Root, "conker-launcher.exe"))
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
        };
        start.ArgumentList.Add("--updated");
        Process.Start(start)?.Dispose();
    }

    /// <summary>
    /// Deletes what an update left: the old files (the old launcher's .exe once it has exited,
    /// so this retries for a while) and an interrupted download.
    /// </summary>
    public async Task CleanAfterUpdateAsync()
    {
        try
        {
            CleanLeftovers();
        }
        catch (IOException)
        {
            // Still in use; the next start removes it.
        }

        if (!File.Exists(CleanupList))
        {
            return;
        }

        List<string> left = [.. File.ReadAllLines(CleanupList).Where(l => l.Length > 0)];
        for (int attempt = 0; attempt < 20 && left.Count > 0; attempt++)
        {
            left.RemoveAll(TryDelete);
            if (left.Count > 0)
            {
                await Task.Delay(500);
            }
        }

        try
        {
            if (left.Count == 0)
            {
                File.Delete(CleanupList);
            }
            else
            {
                File.WriteAllLines(CleanupList, left);
            }
        }
        catch (IOException)
        {
            // Tried again at the next start.
        }
    }

    private void CleanLeftovers()
    {
        File.Delete(Download);
        ImportTools.RemoveFolder(Staging);
    }

    /// <summary>Unpacks the zip into <c>update.tmp\</c>; returns its files' relative paths.</summary>
    private List<string> Unpack(CancellationToken cancel)
    {
        List<string> files = [];
        try
        {
            using ZipArchive zip = ZipFile.OpenRead(Download);
            foreach (ZipArchiveEntry entry in zip.Entries)
            {
                cancel.ThrowIfCancellationRequested();
                string name = entry.FullName.Replace('\\', '/');
                if (name.EndsWith('/'))
                {
                    continue;
                }

                if (Protected.Any(p => name.StartsWith(p, StringComparison.OrdinalIgnoreCase)) || name.EndsWith(OldSuffix, StringComparison.OrdinalIgnoreCase))
                {
                    throw new ImportException($"The update holds {name}, which an update never replaces.");
                }

                string path = ImportTools.SafePath(Staging, name)
                    ?? throw new ImportException($"The update holds a file outside its folder ({name}).");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                entry.ExtractToFile(path);
                files.Add(Path.GetRelativePath(Staging, path));
            }
        }
        catch (InvalidDataException e)
        {
            throw new ImportException($"The update couldn't be unpacked, nothing was changed: {e.Message}");
        }

        foreach (string needed in new[] { "conker.exe", "xemu_apu.dll", "conker-launcher.exe" })
        {
            if (!files.Contains(needed, StringComparer.OrdinalIgnoreCase))
            {
                throw new ImportException($"The update has no {needed}.");
            }
        }

        return files;
    }

    /// <summary>Moves the unpacked files in, renaming the ones they replace; all or nothing.</summary>
    private void Install(List<string> files)
    {
        List<(string Target, string? Old)> done = [];
        try
        {
            foreach (string file in files)
            {
                string target = Path.Combine(folder.Root, file);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                string? old = null;
                if (File.Exists(target))
                {
                    old = FreeOldName(target);
                    File.Move(target, old);
                }

                done.Add((target, old));
                File.Move(Path.Combine(Staging, file), target);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Put back what was replaced so far: the old version stays whole.
            for (int i = done.Count - 1; i >= 0; i--)
            {
                (string target, string? old) = done[i];
                TryDelete(target);
                if (old is not null && File.Exists(old))
                {
                    File.Move(old, target);
                }
            }

            throw new ImportException($"The program files couldn't be replaced, nothing was changed: {e.Message}");
        }

        File.AppendAllLines(CleanupList, done.Where(d => d.Old is not null).Select(d => d.Old!));
    }

    // "conker.exe.update-old", or "...update-old2" while an older one is still in use.
    private static string FreeOldName(string target)
    {
        for (int n = 1; ; n++)
        {
            string old = target + OldSuffix + (n > 1 ? n.ToString(System.Globalization.CultureInfo.InvariantCulture) : "");
            if (!File.Exists(old) || TryDelete(old))
            {
                return old;
            }
        }
    }

    private static bool TryDelete(string path)
    {
        try
        {
            File.Delete(path);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }
}
