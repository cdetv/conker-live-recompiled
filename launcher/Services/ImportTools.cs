using System.Diagnostics;
using System.Runtime.InteropServices;
using SharpCompress.Archives;
using SharpCompress.Common;
using SharpCompress.Readers;

namespace ConkerLauncher.Services;

/// <summary>A problem the player should read (shown as is in the Files tab, in the language shown).</summary>
public sealed class ImportException(Said text) : Exception
{
    public Said Text { get; } = text;

    public override string Message => Text.ToString();
}

/// <summary>What a long job is doing: a line of text and how far it is (0 to 1, or null).</summary>
public sealed record JobProgress(Said Text, double? Fraction);

/// <summary>Byte counting with a progress line at most ten times a second.</summary>
public sealed class ByteProgress(IProgress<JobProgress> progress, Said verb, long total)
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
                S("{0}: {1:0.00} of {2:0.00} GB ({3:0} MB/s)", verb, done / 1e9, total / 1e9, done / seconds / (1 << 20)),
                total > 0 ? (double)done / total : null));
        }
    }
}

/// <summary>Folders, free space and archives, shared by the game and decensor imports.</summary>
public static class ImportTools
{
    private const long SpaceMargin = 64L << 20;

    /// <summary>Stops with a message when the drive holding <paramref name="folder"/> lacks <paramref name="bytes"/>.</summary>
    public static void RequireSpace(string folder, long bytes)
    {
        string full = Path.GetFullPath(folder);
        long free = FreeSpace(full);
        if (free < bytes + SpaceMargin)
        {
            throw new ImportException(S("Not enough free space on {0}: this needs {1:0.00} GB, {2:0.00} GB is free.",
                Path.GetPathRoot(full)!.TrimEnd('\\'), (bytes + SpaceMargin) / 1e9, free / 1e9));
        }
    }

    /// <summary>
    /// Free bytes (for this user) where <paramref name="folder"/> is: asked of the folder itself, not
    /// its drive root. Under Proton the root of Z: is Linux's "/", on a Steam Deck a 5 GB system
    /// partition, while the folder sits on /home (71 GB free: the import said 1.52 GB, 2026-11-20).
    /// On Windows the answer is the drive's, as before.
    /// </summary>
    public static long FreeSpace(string folder)
    {
        string path = Path.GetFullPath(folder);
        while (!Directory.Exists(path) && Path.GetDirectoryName(path) is { } parent)
        {
            path = parent;
        }

        if (OperatingSystem.IsWindows() && GetDiskFreeSpaceExW(path, out ulong available, out _, out _))
        {
            return (long)Math.Min(available, long.MaxValue);
        }

        return new DriveInfo(Path.GetPathRoot(path)!).AvailableFreeSpace;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetDiskFreeSpaceExW(string directory, out ulong available, out ulong total, out ulong totalFree);

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
            throw new ImportException(S("{0} couldn't be opened as an archive: {1}", Path.GetFileName(path), e.Message));
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
            throw new ImportException(S("The archive couldn't be unpacked: {0}", e.Message));
        }
    }

    private static void WriteEntry(IEntry entry, Func<Stream> open, string root, Func<string, string?> target, ByteProgress progress, CancellationToken cancel)
    {
        cancel.ThrowIfCancellationRequested();
        if (entry.IsDirectory || entry.Key is not { } key || target(key.Replace('\\', '/')) is not { } relative)
        {
            return;
        }

        string path = SafePath(root, relative) ?? throw new ImportException(S("The archive holds a file outside its folder ({0}).", key));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using FileStream output = new(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        using Stream input = open();
        CopyStream(input, output, progress, cancel);
    }
}
