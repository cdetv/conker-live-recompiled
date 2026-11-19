using System.Buffers.Binary;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>A file or folder on an Xbox disc image.</summary>
public sealed record XdvdfsEntry(string Path, long Offset, long Size, bool IsDirectory);

/// <summary>
/// Reads an Xbox disc image (XDVDFS): a full Redump image, an XISO, or the Xbox Store version's
/// package (SPEC 9.3).
/// </summary>
/// <remarks>
/// <para>The game partition starts at 0 in an XISO and further in on a full image (where depends on
/// the disc type, so the known places are tried). Its volume descriptor sits 0x10000 into the
/// partition: "MICROSOFT*XBOX*MEDIA", the root folder's sector and size. A folder is a binary
/// tree of entries (u16 left, u16 right in 4-byte steps from the folder's start, u32 sector,
/// u32 size, u8 attributes, u8 name length, name), each padded to 4 bytes. Sectors are 2048
/// bytes, counted from the partition start.</para>
/// <para>The Store package (<see cref="GodPackageStream"/>) holds the volume with its start cut
/// off: the descriptor moved to byte 0, and the folders keep the disc's sector numbers, so data
/// byte 0 is disc sector K (<see cref="CutSectors"/>). K is in no header we know for sure; it is
/// found by trying guesses and, failing those, by searching the data for the root folder, each
/// candidate checked against the whole folder tree (see <see cref="OpenPackage(Stream, IReadOnlyList{long})"/>).</para>
/// </remarks>
public sealed class XdvdfsImage : IDisposable
{
    /// <summary>K on the user's Store copy (2026-11-16): the second guess after the header's.</summary>
    public const long KnownStoreCut = 956_202;

    private const int SectorSize = 2048;
    private const long VolumeOffset = 0x10000;
    private const byte DirectoryAttribute = 0x10;
    private const uint MaxFolderSize = 16 << 20;
    private const string GameXbe = "default.xbe";
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

    // XISO, then full images of XGD1, XGD2 and XGD3 discs.
    private static readonly long[] PartitionOffsets = [0, 0x18300000, 0xFD90000, 0x2080000];

    private readonly Stream stream;
    private readonly long partition;
    private readonly uint rootSector;
    private readonly uint rootSize;

    private XdvdfsImage(Stream stream, long partition, uint rootSector, uint rootSize)
    {
        this.stream = stream;
        this.partition = partition;
        this.rootSector = rootSector;
        this.rootSize = rootSize;
    }

    /// <summary>Disc sectors cut off before the data (a Store package's K); 0 for a disc image.</summary>
    public long CutSectors => partition < 0 ? -partition / SectorSize : 0;

    /// <summary>
    /// Opens the Xbox Store version's package: the header's guess for K, then the known K, then
    /// the search. Stops with <see cref="GodPackageStream.DamagedMessage"/> when the volume can't
    /// be read. The image owns <paramref name="package"/> (disposed with it, or here on failure).
    /// </summary>
    public static XdvdfsImage OpenPackage(GodPackageStream package) =>
        OpenPackage(package, [2L * (package.DataBlockOffset - 1), KnownStoreCut]);

    /// <summary>
    /// Opens a volume whose descriptor is at byte 0 of <paramref name="data"/> and whose start is
    /// cut off: tries each of <paramref name="guesses"/> for K, then searches the data for the
    /// root folder. A K counts only when every entry of the folder tree lies inside the data and
    /// <c>default.xbe</c> starts with "XBEH". Tests pass wrong guesses to reach the search.
    /// </summary>
    public static XdvdfsImage OpenPackage(Stream data, IReadOnlyList<long> guesses)
    {
        try
        {
            byte[] head = new byte[28];
            data.Position = 0;
            if (data.Length < SectorSize || data.ReadAtLeast(head, head.Length, false) != head.Length
                || !head.AsSpan(0, Magic.Length).SequenceEqual(Magic))
            {
                throw new ImportException(GodPackageStream.DamagedMessage);
            }

            uint rootSector = BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(20));
            uint rootSize = BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(24));
            HashSet<long> tried = [];
            foreach (long k in guesses)
            {
                if (tried.Add(k) && TryCut(data, k, rootSector, rootSize) is { } image)
                {
                    return image;
                }
            }

            foreach (long k in FindRootCandidates(data, rootSector, rootSize))
            {
                if (tried.Add(k) && TryCut(data, k, rootSector, rootSize) is { } image)
                {
                    return image;
                }
            }

            throw new ImportException(GodPackageStream.DamagedMessage);
        }
        catch
        {
            data.Dispose();
            throw;
        }
    }

    /// <summary>Opens <paramref name="path"/>; null when it isn't an Xbox disc image.</summary>
    public static XdvdfsImage? Open(string path)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        byte[] head = new byte[28];
        foreach (long partition in PartitionOffsets)
        {
            if (partition + VolumeOffset + head.Length > stream.Length)
            {
                continue;
            }

            stream.Position = partition + VolumeOffset;
            stream.ReadExactly(head);
            if (head.AsSpan(0, Magic.Length).SequenceEqual(Magic))
            {
                return new XdvdfsImage(stream, partition,
                    BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(20)),
                    BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(24)));
            }
        }

        stream.Dispose();
        return null;
    }

    /// <summary>Every file and folder on the disc, folders before what they hold.</summary>
    public List<XdvdfsEntry> ListAll()
    {
        List<XdvdfsEntry> entries = [];
        try
        {
            ListFolder(rootSector, rootSize, "", entries, 0);
        }
        catch (InvalidDataException)
        {
            throw new ImportException(partition < 0
                ? GodPackageStream.DamagedMessage
                : "The disc image is damaged: a folder lies outside it.");
        }

        return entries;
    }

    /// <summary>Copies one file's bytes to <paramref name="destination"/>.</summary>
    public void Copy(XdvdfsEntry entry, Stream destination, Action<long> copied, CancellationToken cancel)
    {
        stream.Position = entry.Offset;
        byte[] buffer = new byte[1 << 20];
        long left = entry.Size;
        while (left > 0)
        {
            cancel.ThrowIfCancellationRequested();
            int n = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
            if (n == 0)
            {
                throw new ImportException("The disc image ends early; it may be damaged or incomplete.");
            }

            destination.Write(buffer, 0, n);
            left -= n;
            copied(n);
        }
    }

    public byte[] ReadAll(XdvdfsEntry entry)
    {
        using MemoryStream memory = new((int)entry.Size);
        Copy(entry, memory, _ => { }, CancellationToken.None);
        return memory.ToArray();
    }

    public void Dispose() => stream.Dispose();

    /// <summary>
    /// The image with the start cut off by <paramref name="k"/> sectors, if the folder tree reads
    /// whole with it and <c>default.xbe</c> starts with "XBEH"; else null.
    /// </summary>
    private static XdvdfsImage? TryCut(Stream data, long k, uint rootSector, uint rootSize)
    {
        if (k <= 0 || k > rootSector)
        {
            return null;
        }

        XdvdfsImage image = new(data, -k * SectorSize, rootSector, rootSize);
        try
        {
            List<XdvdfsEntry> entries = [];
            image.ListFolder(rootSector, rootSize, "", entries, 0);
            XdvdfsEntry? xbe = entries.Find(e => !e.IsDirectory && e.Path.Equals(GameXbe, StringComparison.OrdinalIgnoreCase));
            if (xbe is null || xbe.Size < 4 || entries.Exists(e => e.Offset < 0 || e.Offset + e.Size > data.Length))
            {
                return null;
            }

            byte[] magic = new byte[4];
            data.Position = xbe.Offset;
            data.ReadExactly(magic);
            return "XBEH"u8.SequenceEqual(magic) ? image : null;
        }
        catch (Exception e) when (e is InvalidDataException or EndOfStreamException)
        {
            return null;
        }
    }

    /// <summary>
    /// K candidates from every sector of <paramref name="data"/> holding a folder entry named
    /// <c>default.xbe</c> (the root folder holds it): K = root sector - that sector, or less by the
    /// sectors before it in a root folder longer than one sector. Reads the whole data once at most.
    /// </summary>
    private static IEnumerable<long> FindRootCandidates(Stream data, uint rootSector, uint rootSize)
    {
        byte[][] names = [Encoding.ASCII.GetBytes(GameXbe), Encoding.ASCII.GetBytes(GameXbe.ToUpperInvariant())];
        long rootSectors = Math.Max(1, ((long)rootSize + SectorSize - 1) / SectorSize);
        byte[] chunk = new byte[4 << 20];
        for (long at = 0; at < data.Length; at += chunk.Length)
        {
            data.Position = at;
            int n = data.ReadAtLeast(chunk, chunk.Length, false);
            foreach (byte[] name in names)
            {
                for (int from = 0; from < n;)
                {
                    int hit = chunk.AsSpan(from, n - from).IndexOf(name);
                    if (hit < 0)
                    {
                        break;
                    }

                    int pos = from + hit;
                    from = pos + 1;
                    int entry = pos - 14;
                    if (entry < 0 || entry % 4 != 0 || chunk[pos - 1] != name.Length)
                    {
                        continue;
                    }

                    long sector = (at + entry) / SectorSize;
                    for (long j = 0; j < rootSectors && j <= sector; j++)
                    {
                        yield return rootSector - (sector - j);
                    }
                }
            }
        }
    }

    private void ListFolder(uint sector, uint size, string prefix, List<XdvdfsEntry> entries, int depth)
    {
        if (size == 0 || depth > 32)
        {
            return;
        }

        // Bounds first: a wrong K (or a damaged image) points anywhere, with any size.
        long offset = partition + (long)sector * SectorSize;
        if (size > MaxFolderSize || offset < 0 || offset + size > stream.Length)
        {
            throw new InvalidDataException("A folder lies outside the image.");
        }

        byte[] folder = new byte[size];
        stream.Position = offset;
        stream.ReadExactly(folder);

        // Walk the tree without recursion on left/right (a folder can be long).
        Stack<int> pending = new();
        HashSet<int> seen = [];
        pending.Push(0);
        while (pending.Count > 0)
        {
            int at = pending.Pop();
            if (at + 14 > folder.Length || !seen.Add(at))
            {
                continue;
            }

            ReadOnlySpan<byte> e = folder.AsSpan(at);
            ushort left = BinaryPrimitives.ReadUInt16LittleEndian(e);
            ushort right = BinaryPrimitives.ReadUInt16LittleEndian(e[2..]);
            if (left == 0xFFFF && right == 0xFFFF)
            {
                continue; // padding, or an empty folder
            }

            uint start = BinaryPrimitives.ReadUInt32LittleEndian(e[4..]);
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(e[8..]);
            byte attributes = e[12];
            int nameLength = e[13];
            if (at + 14 + nameLength > folder.Length)
            {
                continue;
            }

            string name = Encoding.Latin1.GetString(e.Slice(14, nameLength));
            string path = prefix.Length == 0 ? name : prefix + "\\" + name;
            bool isDirectory = (attributes & DirectoryAttribute) != 0;
            entries.Add(new XdvdfsEntry(path, partition + (long)start * SectorSize, isDirectory ? 0 : length, isDirectory));
            if (isDirectory)
            {
                ListFolder(start, length, path, entries, depth + 1);
            }

            if (right != 0 && right != 0xFFFF)
            {
                pending.Push(right * 4);
            }

            if (left != 0 && left != 0xFFFF)
            {
                pending.Push(left * 4);
            }
        }
    }
}
