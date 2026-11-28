using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// The game assets in the user files (to-do 43, SPEC 9.3): <c>assets\</c>, made on this PC from
/// the player's own game files by <c>conker.exe --carve</c> (src/caff_cache.c) and never shipped.
/// The launcher makes them right after an import, and once at start when they are stale (an
/// update from an older version brings no import; game files changed). <c>--carve --check</c>
/// says which, from the folder listing and <c>assets\stamp.json</c>, in a moment.
/// </summary>
public static class AssetCache
{
    public static string Folder => Path.Combine(SaveFolder.Root, "assets");

    public static string StampFile => Path.Combine(Folder, "stamp.json");

    /// <summary>Has a carve ever finished here (the stamp is written last)?</summary>
    public static bool Prepared => File.Exists(StampFile);

    /// <summary>The files in the folder and their size together.</summary>
    public static (int Files, long Bytes) Size()
    {
        try
        {
            if (!Directory.Exists(Folder))
            {
                return (0, 0);
            }

            FileInfo[] files = new DirectoryInfo(Folder).GetFiles("*", SearchOption.AllDirectories);
            return (files.Length, files.Sum(f => f.Length));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return (0, 0);
        }
    }

    /// <summary>Deletes the folder (the launcher makes it again at its next start).</summary>
    public static void Delete()
    {
        if (Directory.Exists(Folder))
        {
            Directory.Delete(Folder, recursive: true);
        }
    }

    /// <summary>
    /// <c>conker.exe --carve --check</c>: true = stale, false = up to date; null = can't tell
    /// (no conker.exe, no game files, it didn't answer).
    /// </summary>
    public static async Task<bool?> IsStaleAsync(GameFolder folder, CancellationToken cancel)
    {
        if (!File.Exists(folder.GameExe))
        {
            return null;
        }

        try
        {
            using Process process = Start(folder, check: true);
            try
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                timeout.CancelAfter(TimeSpan.FromSeconds(30));
                Task<string> output = process.StandardOutput.ReadToEndAsync(timeout.Token);
                Task<string> errors = process.StandardError.ReadToEndAsync(timeout.Token);
                await process.WaitForExitAsync(timeout.Token);
                await Task.WhenAll(output, errors);
            }
            finally
            {
                OwnProcesses.Forget(process);
            }

            return process.ExitCode switch { 0 => false, 3 => true, _ => null };
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception or OperationCanceledException
                                       or InvalidOperationException)
        {
            return null;
        }
    }

    /// <summary>
    /// <c>conker.exe --carve</c>, its <c>PROGRESS done total name</c> lines as progress. A cancel
    /// stops it (the stamp is written last, so the next check finds the cache stale). Throws an
    /// <see cref="IOException"/> with the reason when it fails.
    /// </summary>
    public static async Task CarveAsync(GameFolder folder, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        if (!File.Exists(folder.GameExe))
        {
            throw new SaidIOException(S("conker.exe is missing from the launcher's folder."));
        }

        progress.Report(new JobProgress(S("Preparing game assets"), null));
        Process process;
        try
        {
            process = Start(folder, check: false);
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            throw new SaidIOException(S("conker.exe didn't start: {0}", e.Message), e);
        }

        using (process)
        {
            Task<string> errors = process.StandardError.ReadToEndAsync(CancellationToken.None);
            try
            {
                string? line;
                while ((line = await process.StandardOutput.ReadLineAsync(cancel)) is not null)
                {
                    Report(line, progress);
                }

                await process.WaitForExitAsync(cancel);
            }
            catch (OperationCanceledException)
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch (InvalidOperationException)
                {
                    // Ended meanwhile.
                }

                throw;
            }
            finally
            {
                OwnProcesses.Forget(process);
            }

            if (process.ExitCode != 0)
            {
                // conker.exe's own reason stays English, as its log does.
                string? why = (await errors).Split('\n').Select(l => l.Trim())
                    .LastOrDefault(l => l.StartsWith("[CAFF] ", StringComparison.Ordinal))?["[CAFF] ".Length..];
                throw why is not null ? new IOException(why) : new SaidIOException(S("conker.exe ended with {0}.", process.ExitCode));
            }
        }
    }

    private static void Report(string line, IProgress<JobProgress> progress)
    {
        // "PROGRESS <done> <total> <package>"
        string[] parts = line.Split(' ', 4);
        if (parts.Length < 3 || parts[0] != "PROGRESS"
            || !int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int done)
            || !int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int total))
        {
            return;
        }

        string name = parts.Length > 3 ? parts[3].Trim() : "";
        progress.Report(new JobProgress(
            name.Length > 0 ? S("Preparing game assets ({0} of {1}: {2})", done, total, name) : S("Preparing game assets"),
            total > 0 ? (double)done / total : null));
    }

    private static Process Start(GameFolder folder, bool check)
    {
        var start = new ProcessStartInfo(folder.GameExe)
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        start.ArgumentList.Add("--carve");
        if (check)
        {
            start.ArgumentList.Add("--check");
        }

        start.ArgumentList.Add("--game-dir");
        start.ArgumentList.Add(folder.GameDir);
        start.ArgumentList.Add("--save-dir");
        start.ArgumentList.Add(SaveFolder.Root);
        return OwnProcesses.Start(start) ?? throw new SaidIOException(S("conker.exe didn't start."));
    }
}
