using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace ConkerLauncher.Services;

/// <summary>
/// The texture pack kit (to-do 238, SPEC 9.3): <c>conker.exe --texture-export</c> writes every
/// texture of the player's own game files that a replacement can stand in for, once, under the
/// extractor's file name in its package's own folder below <c>aid</c> (its tree:
/// <c>zpackage\Level\Bank\&lt;name&gt;.png</c>), into <c>textures\dump</c> (user 2026-11-26):
/// inside the <c>textures</c> folder, which the game never reads the dump from, so the dump itself
/// replaces nothing. A pack is a folder in <c>textures\packs</c> holding edited copies under the
/// same names and folders (to-do 288: the game finds their hashes through the name table; the hash
/// stays in the dump's <c>textures.tsv</c>). Made on this PC from the player's files: never shipped
/// (SPEC 3.3); updates replace their own files only, so the dump stays. PNGs already there are
/// kept, so a stopped export goes on where it was.
/// </summary>
public static partial class TextureExport
{
    public static string Folder(GameFolder folder) => Path.Combine(TexturesFolder(folder), "dump");

    /// <summary>Where the game reads replacement PNGs from (RECOMP_TEX_REPLACE's default).</summary>
    public static string TexturesFolder(GameFolder folder) => Path.Combine(folder.Root, "textures");

    /// <summary>The list an export writes last (one row per picture).</summary>
    public static string ListFile(GameFolder folder) => Path.Combine(Folder(folder), "textures.tsv");

    /// <summary>The PNGs in the export folder and their size together; (0, 0) when none.</summary>
    public static (int Pngs, long Bytes) Size(GameFolder folder)
    {
        try
        {
            if (!Directory.Exists(Folder(folder)))
            {
                return (0, 0);
            }

            FileInfo[] files = new DirectoryInfo(Folder(folder)).GetFiles("*.png", SearchOption.AllDirectories);
            return (files.Length, files.Sum(f => f.Length));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return (0, 0);
        }
    }

    /// <summary>What a finished export did (its DONE line).</summary>
    public sealed record Result(int Total, int Written, int Kept, int Failed, double MegaBytes);

    [GeneratedRegex(@"^DONE \d+ packages \(\d+ failed\), (\d+) pictures a pack can replace: (\d+) PNGs written \(([\d.]+) MB\), (\d+) there already, (\d+) failed")]
    private static partial Regex DoneLine();

    /// <summary>
    /// <c>conker.exe --texture-export</c>, its <c>PROGRESS done total name</c> lines as progress.
    /// A cancel stops it (each PNG is written whole or not at all; the next export goes on).
    /// Throws an <see cref="IOException"/> with the reason when it fails.
    /// </summary>
    public static async Task<Result> ExportAsync(GameFolder folder, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        if (!File.Exists(folder.GameExe))
        {
            throw new SaidIOException(S("conker.exe is missing from the launcher's folder."));
        }

        RemoveLeftovers(Folder(folder));
        progress.Report(new JobProgress(S("Exporting textures"), null));
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
        start.ArgumentList.Add("--texture-export");
        start.ArgumentList.Add(Folder(folder));
        start.ArgumentList.Add("--game-dir");
        start.ArgumentList.Add(folder.GameDir);

        Process process;
        try
        {
            process = OwnProcesses.Start(start) ?? throw new SaidIOException(S("conker.exe didn't start."));
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            throw new SaidIOException(S("conker.exe didn't start: {0}", e.Message), e);
        }

        using (process)
        {
            Task<string> errors = process.StandardError.ReadToEndAsync(CancellationToken.None);
            Result? result = null;
            try
            {
                string? line;
                while ((line = await process.StandardOutput.ReadLineAsync(cancel)) is not null)
                {
                    if (DoneLine().Match(line) is { Success: true } m)
                    {
                        result = new Result(Int(m.Groups[1].Value), Int(m.Groups[2].Value), Int(m.Groups[4].Value),
                                            Int(m.Groups[5].Value),
                                            double.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        Report(line, progress);
                    }
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

            // Exit 1 with a DONE line: some PNGs failed, the rest are there (result.Failed says how many).
            if (result is null)
            {
                // conker.exe's own reason stays English, as its log does.
                string? why = (await errors).Split('\n').Select(l => l.Trim())
                    .LastOrDefault(l => l.StartsWith("[TEX] ", StringComparison.Ordinal) || l.StartsWith("[CAFF] ", StringComparison.Ordinal))
                    is { } last ? last[(last.IndexOf(' ') + 1)..] : null;
                throw why is not null ? new IOException(why) : new SaidIOException(S("conker.exe ended with {0}.", process.ExitCode));
            }

            return result;
        }
    }

    private static int Int(string s) => int.Parse(s, NumberStyles.Integer, CultureInfo.InvariantCulture);

    /// <summary>The <c>.tmp</c> files a stopped export can leave (a PNG being written when it was stopped).</summary>
    private static void RemoveLeftovers(string root)
    {
        try
        {
            if (!Directory.Exists(root))
            {
                return;
            }

            foreach (string tmp in Directory.EnumerateFiles(root, "*.png.tmp", SearchOption.AllDirectories))
            {
                File.Delete(tmp);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Still in use or read-only: the export writes its own .tmp anew anyway.
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
            name.Length > 0 ? S("Exporting textures ({0} of {1}: {2})", done, total, name) : S("Exporting textures"),
            total > 0 ? (double)done / total : null));
    }
}
