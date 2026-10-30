using System.Diagnostics;
using SharpCompress.Archives;
using SharpCompress.Common;
using SharpCompress.Readers;

namespace ConkerLauncher.Services;

/// <summary>A problem the player should read (shown as is in the Files tab).</summary>
public sealed class ImportException(string message) : Exception(message);

/// <summary>What a long job is doing: a line of text and how far it is (0 to 1, or null).</summary>
public sealed record JobProgress(string Text, double? Fraction);

/// <summary>Byte counting with a progress line at most ten times a second.</summary>
public sealed class ByteProgress(IProgress<JobProgress> progress, string verb, long total)
{
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private long done;
    private long lastReport = -1000;

    public void Add(long bytes)
    {
        done += bytes;
        long now = clock.ElapsedMilliseconds;
        if (now - lastReport >= 100 || done >= total)
        {
            lastReport = now;
            double seconds = Math.Max(clock.Elapsed.TotalSeconds, 0.001);
            progress.Report(new JobProgress(
                $"{verb}: {Gb(done)} of {Gb(total)} GB ({done / seconds / (1 << 20):0} MB/s)",
                total > 0 ? (double)done / total : null));
        }
    }

    public static string Gb(long bytes) => (bytes / 1e9).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
}

/// <summary>Folders, free space and archives, shared by the game and decensor imports.</summary>
public static class ImportTools
{
    private const long SpaceMargin = 64L << 20;

    /// <summary>Stops with a message when the drive holding <paramref name="folder"/> lacks <paramref name="bytes"/>.</summary>
    public static void RequireSpace(string folder, long bytes)
    {
        DriveInfo drive = new(Path.GetPathRoot(Path.GetFullPath(folder))!);
        if (drive.AvailableFreeSpace < bytes + SpaceMargin)
        {
            throw new ImportException(
                $"Not enough free space on {drive.Name.TrimEnd('\\')}: this needs {ByteProgress.Gb(bytes + SpaceMargin)} GB, " +
                $"{ByteProgress.Gb(drive.AvailableFreeSpace)} GB is free.");
        }
    }

    /// <summary>
    /// Removes a folder the launcher made. A link (a development build's <c>game</c> junction) is
    /// removed as a link: what it points to is never touched.
    /// </summary>
    public static void RemoveFolder(string path)
    {
        DirectoryInfo info = new(path);
        if (!info.Exists)
        {
            return;
        }

        if (info.Attributes.HasFlag(FileAttributes.ReparsePoint))
        {
            info.Delete();
            return;
        }

        foreach (FileInfo file in info.EnumerateFiles("*", SearchOption.AllDirectories))
        {
            if (file.IsReadOnly)
            {
                file.IsReadOnly = false;
            }
        }

        info.Delete(true);
    }

    /// <summary>Puts a finished staging folder in place of <paramref name="target"/>.</summary>
    public static void Swap(string staging, string target)
    {
        RemoveFolder(target);
        Directory.Move(staging, target);
    }

    /// <summary>Copies a stream with progress and cancel, in 1 MB steps.</summary>
    public static void CopyStream(Stream source, Stream destination, ByteProgress progress, CancellationToken cancel)
    {
        byte[] buffer = new byte[1 << 20];
        int n;
        while ((n = source.Read(buffer, 0, buffer.Length)) > 0)
        {
            cancel.ThrowIfCancellationRequested();
            destination.Write(buffer, 0, n);
            progress.Add(n);
        }
    }

    /// <summary>A path inside <paramref name="root"/> for an archive entry; null if it would land outside.</summary>
    public static string? SafePath(string root, string relative)
    {
        string full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', '\\').TrimStart('\\')));
        string rootFull = Path.GetFullPath(root).TrimEnd('\\') + "\\";
        return full.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase) ? full : null;
    }

    public static IArchive OpenArchive(string path)
    {
        try
        {
            return ArchiveFactory.OpenArchive(path, new ReaderOptions());
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            throw new ImportException($"{Path.GetFileName(path)} couldn't be opened as an archive: {e.Message}");
        }
    }

    /// <summary>The archive's file entries' names (forward slashes), sizes.</summary>
    public static List<(string Key, long Size)> ListFiles(IArchive archive) =>
        [.. archive.Entries.Where(e => !e.IsDirectory && e.Key is not null).Select(e => (e.Key!.Replace('\\', '/'), e.Size))];

    /// <summary>
    /// Unpacks the files <paramref name="target"/> gives a path for (relative to
    /// <paramref name="root"/>), in archive order, which keeps solid 7z archives fast.
    /// </summary>
    public static void Extract(IArchive archive, string root, Func<string, string?> target, ByteProgress progress, CancellationToken cancel)
    {
        try
        {
            // 7z and solid archives (rar often is) only unpack in one pass; zip entries open one by one.
            if (archive.IsSolid || archive.Type == ArchiveType.SevenZip)
            {
                using IReader reader = archive.ExtractAllEntries();
                while (reader.MoveToNextEntry())
                {
                    WriteEntry(reader.Entry, reader.OpenEntryStream, root, target, progress, cancel);
                }
            }
            else
            {
                foreach (IArchiveEntry entry in archive.Entries)
                {
                    WriteEntry(entry, entry.OpenEntryStream, root, target, progress, cancel);
                }
            }
        }
        catch (SharpCompressException e)
        {
            throw new ImportException($"The archive couldn't be unpacked: {e.Message}");
        }
    }

    private static void WriteEntry(IEntry entry, Func<Stream> open, string root, Func<string, string?> target, ByteProgress progress, CancellationToken cancel)
    {
        cancel.ThrowIfCancellationRequested();
        if (entry.IsDirectory || entry.Key is not { } key || target(key.Replace('\\', '/')) is not { } relative)
        {
            return;
        }

        string path = SafePath(root, relative) ?? throw new ImportException($"The archive holds a file outside its folder ({key}).");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using FileStream output = new(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        using Stream input = open();
        CopyStream(input, output, progress, cancel);
    }
}
