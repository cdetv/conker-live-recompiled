using System.Buffers.Binary;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>A file or folder on an Xbox disc image.</summary>
public sealed record XdvdfsEntry(string Path, long Offset, long Size, bool IsDirectory);

/// <summary>
/// Reads an Xbox disc image (XDVDFS): a full Redump image or an XISO (SPEC 9.3).
/// </summary>
/// <remarks>
/// The game partition starts at 0 in an XISO and further in on a full image (where depends on
/// the disc type, so the known places are tried). Its volume descriptor sits 0x10000 into the
/// partition: "MICROSOFT*XBOX*MEDIA", the root folder's sector and size. A folder is a binary
/// tree of entries (u16 left, u16 right in 4-byte steps from the folder's start, u32 sector,
/// u32 size, u8 attributes, u8 name length, name), each padded to 4 bytes. Sectors are 2048
/// bytes, counted from the partition start.
/// </remarks>
public sealed class XdvdfsImage : IDisposable
{
    private const int SectorSize = 2048;
    private const long VolumeOffset = 0x10000;
    private const byte DirectoryAttribute = 0x10;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

    // XISO, then full images of XGD1, XGD2 and XGD3 discs.
    private static readonly long[] PartitionOffsets = [0, 0x18300000, 0xFD90000, 0x2080000];

    private readonly FileStream stream;
    private readonly long partition;
    private readonly uint rootSector;
    private readonly uint rootSize;

    private XdvdfsImage(FileStream stream, long partition, uint rootSector, uint rootSize)
    {
        this.stream = stream;
        this.partition = partition;
        this.rootSector = rootSector;
        this.rootSize = rootSize;
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
        ListFolder(rootSector, rootSize, "", entries, 0);
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

    private void ListFolder(uint sector, uint size, string prefix, List<XdvdfsEntry> entries, int depth)
    {
        if (size == 0 || depth > 32)
        {
            return;
        }

        byte[] folder = new byte[size];
        stream.Position = partition + (long)sector * SectorSize;
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
