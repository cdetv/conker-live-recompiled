using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// Reads the Xbox Store version's game package (SPEC 9.3): the header file <c>DefaultPackage</c>
/// and its <c>DefaultPackage.data\Data0000..</c> files, shown as one read-only stream of the
/// game's data with the hash blocks left out.
/// </summary>
/// <remarks>
/// <para>Header (big-endian unless said): "PIRS" at 0, content type 0x5000 (an original Xbox
/// game) at 0x344, title id at 0x360, the volume descriptor at 0x379 (its size byte 0x24; at
/// 0x37D the root hash = SHA-1 of Data0000's master block; at 0x395 the data block offset, 3 bytes
/// little-endian), the Data file count at 0x39D, their total size at 0x3A1, descriptor type 1 at
/// 0x3A9.</para>
/// <para>A Data file is 4 KiB blocks: block 0 = the master hash table (one SHA-1 per group's
/// sub-table; at 4060 the SHA-1 of the next file's master block), then up to 203 groups of one
/// sub-table block (one SHA-1 per data block) and 204 data blocks. Every file but the last is
/// full (41,616 blocks). Not encrypted: the data blocks in order are the game's volume (read by
/// <see cref="XdvdfsImage"/>). Checked on the user's never-started install (2026-11-16).</para>
/// </remarks>
public sealed class GodPackageStream : Stream
{
    public const string PackageName = "DefaultPackage";

    /// <summary>The bad files message (user-approved wording, 2026-11-16).</summary>
    public static Said DamagedMessage =>
        S("These files don't read as a complete Xbox Store version of the game. In the Xbox app, choose " +
          "Manage > Files > Verify and repair (or reinstall it), then try again.");

    /// <summary>The missing files message (user-approved wording, 2026-11-16).</summary>
    public static Said PartMessage =>
        S("This folder holds only part of the Xbox Store version. In the Xbox app, use Verify and repair, then try again.");

    public const int HeaderLength = 0x3AD;
    private const int Block = 4096;
    private const int PerGroup = 204;
    private const int GroupsPerFile = 203;
    private const int HashSize = 20;
    private const int NextMasterAt = GroupsPerFile * HashSize;
    private const long FullFileBlocks = 1 + (long)GroupsPerFile * (PerGroup + 1);
    private const long DataPerFile = (long)GroupsPerFile * PerGroup * Block;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("PIRS");

    private readonly FileStream[] files;
    private readonly byte[] rootHash;
    private readonly long length;
    private readonly long lastFileDataBlocks;
    private readonly byte[] groupBuffer = new byte[PerGroup * Block];
    private readonly byte[] subTable = new byte[Block];
    private byte[][]? masters;
    private long subTableKey = -1;
    private long position;

    private GodPackageStream(FileStream[] files, byte[] rootHash, long lastFileDataBlocks, uint titleId, int dataBlockOffset)
    {
        this.files = files;
        this.rootHash = rootHash;
        this.lastFileDataBlocks = lastFileDataBlocks;
        TitleId = titleId;
        DataBlockOffset = dataBlockOffset;
        length = (files.Length - 1) * DataPerFile + lastFileDataBlocks * Block;
    }

    /// <summary>The game's title id from the header (Conker: 0x4D530051).</summary>
    public uint TitleId { get; }

    /// <summary>
    /// The header's data block offset (4 KiB blocks). On the user's copy the cut-off start of the
    /// volume, K sectors, was 2 x (this - 1); <see cref="XdvdfsImage"/> takes it only as a first guess.
    /// </summary>
    public int DataBlockOffset { get; }

    /// <summary>Hash-check every data block as it is read (an import's full check).</summary>
    public bool VerifyReads { get; set; }

    /// <summary>Number of Data files.</summary>
    public int FileCount => files.Length;

    public override bool CanRead => true;

    public override bool CanSeek => true;

    public override bool CanWrite => false;

    public override long Length => length;

    public override long Position
    {
        get => position;
        set => position = value >= 0 ? value : throw new ArgumentOutOfRangeException(nameof(value));
    }

    /// <summary>True when <paramref name="header"/> (the first <see cref="HeaderLength"/> bytes) is an original Xbox game package.</summary>
    public static bool IsPackageHeader(ReadOnlySpan<byte> header) =>
        header.Length >= HeaderLength
        && header[..4].SequenceEqual(Magic)
        && BinaryPrimitives.ReadUInt32BigEndian(header[0x344..]) == 0x5000
        && header[0x379] == 0x24
        && BinaryPrimitives.ReadUInt32BigEndian(header[0x3A9..]) == 1;

    /// <summary>Reads a package header; null when <paramref name="path"/> isn't one (or can't be read).</summary>
    public static byte[]? ReadHeader(string path)
    {
        try
        {
            using FileStream f = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            byte[] header = new byte[HeaderLength];
            return f.Length >= header.Length && f.ReadAtLeast(header, header.Length, false) == header.Length && IsPackageHeader(header)
                ? header : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>
    /// Opens the package whose header file is <paramref name="packageFile"/>. Stops with
    /// <see cref="PartMessage"/> when a Data file is missing or cut short.
    /// </summary>
    public static GodPackageStream Open(string packageFile)
    {
        byte[] header = ReadHeader(packageFile)
            ?? throw new ImportException(S("{0} isn't an Xbox Store version package.", Path.GetFileName(packageFile)));
        int count = (int)Math.Min(BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(0x39D)), int.MaxValue);
        long total = (long)Math.Min(BinaryPrimitives.ReadUInt64BigEndian(header.AsSpan(0x3A1)), long.MaxValue);
        if (count is < 1 or > 9999)
        {
            throw new ImportException(DamagedMessage);
        }

        string dataFolder = packageFile + ".data";
        List<FileStream> opened = [];
        try
        {
            long sum = 0;
            long lastBlocks = 0;
            for (int n = 0; n < count; n++)
            {
                string path = Path.Combine(dataFolder, $"Data{n:D4}");
                if (!File.Exists(path))
                {
                    throw new ImportException(PartMessage);
                }

                FileStream file = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16);
                opened.Add(file);
                long blocks = file.Length / Block;
                bool fits = file.Length % Block == 0 && (n < count - 1 ? blocks == FullFileBlocks : blocks is > 1 and <= FullFileBlocks);
                if (!fits)
                {
                    throw new ImportException(PartMessage);
                }

                sum += file.Length;
                lastBlocks = blocks;
            }

            if (sum != total)
            {
                throw new ImportException(PartMessage);
            }

            // Data blocks in the last file: all but the master block and one sub-table per started group.
            long groups = (lastBlocks - 1 + PerGroup) / (PerGroup + 1);
            return new GodPackageStream([.. opened], header.AsSpan(0x37D, HashSize).ToArray(), lastBlocks - 1 - groups,
                BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(0x360)),
                header[0x395] | (header[0x396] << 8) | (header[0x397] << 16));
        }
        catch
        {
            foreach (FileStream file in opened)
            {
                file.Dispose();
            }

            throw;
        }
    }

    /// <summary>
    /// The quick check (when a package is picked): the chain of master blocks from the header's
    /// root hash through every Data file, then <paramref name="groups"/> whole groups spread over
    /// the package (first, ..., last). Stops with <see cref="DamagedMessage"/> on a mismatch.
    /// </summary>
    public void CheckSample(int groups = 3)
    {
        LoadMasters();
        long fullGroups = (long)(files.Length - 1) * GroupsPerFile;
        long all = fullGroups + (lastFileDataBlocks + PerGroup - 1) / PerGroup;
        long dataPos = position;
        bool verify = VerifyReads;
        byte[] scratch = new byte[1 << 16];
        try
        {
            VerifyReads = true;
            for (int k = 0; k < groups && all > 0; k++)
            {
                long group = groups == 1 ? 0 : all * k / (groups - 1) - (k == groups - 1 ? 1 : 0);
                group = Math.Clamp(group, 0, all - 1);
                long start = group * PerGroup * Block;
                position = start;
                long left = Math.Min((long)PerGroup * Block, length - start);
                while (left > 0)
                {
                    int n = Read(scratch, 0, (int)Math.Min(left, scratch.Length));
                    if (n == 0)
                    {
                        throw new ImportException(DamagedMessage);
                    }

                    left -= n;
                }
            }
        }
        finally
        {
            VerifyReads = verify;
            position = dataPos;
        }
    }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        int done = 0;
        while (done < buffer.Length && position < length)
        {
            long file = position / DataPerFile;
            long rest = position % DataPerFile;
            long block = rest / Block;
            long group = block / PerGroup;
            int index = (int)(block % PerGroup);
            int within = (int)(rest % Block);

            // A group's data blocks lie one after another on disk: read up to its end at once.
            int run = (int)Math.Min(Math.Min(buffer.Length - done, (long)(PerGroup - index) * Block - within), length - position);
            long at = Block * (1 + group * (PerGroup + 1) + 1 + index);
            if (VerifyReads)
            {
                int blocks = (within + run + Block - 1) / Block;
                Span<byte> whole = groupBuffer.AsSpan(0, blocks * Block);
                ReadAt((int)file, at, whole);
                CheckBlocks((int)file, group, index, whole);
                whole.Slice(within, run).CopyTo(buffer[done..]);
            }
            else
            {
                ReadAt((int)file, at + within, buffer.Slice(done, run));
            }

            done += run;
            position += run;
        }

        return done;
    }

    public override long Seek(long offset, SeekOrigin origin) => Position = origin switch
    {
        SeekOrigin.Begin => offset,
        SeekOrigin.Current => position + offset,
        _ => length + offset,
    };

    public override void Flush()
    {
    }

    public override void SetLength(long value) => throw new NotSupportedException();

    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            foreach (FileStream file in files)
            {
                file.Dispose();
            }
        }

        base.Dispose(disposing);
    }

#pragma warning disable CA5350 // SHA-1 is what the package's hash tables hold; it checks for damage, not tampering.
    private static bool HashEquals(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expected)
    {
        Span<byte> hash = stackalloc byte[HashSize];
        SHA1.HashData(data, hash);
        return hash.SequenceEqual(expected[..HashSize]);
    }
#pragma warning restore CA5350

    private void ReadAt(int file, long at, Span<byte> into)
    {
        FileStream stream = files[file];
        stream.Position = at;
        if (stream.ReadAtLeast(into, into.Length, false) != into.Length)
        {
            throw new ImportException(PartMessage);
        }
    }

    /// <summary>Reads every master block and checks the chain from the header's root hash.</summary>
    private void LoadMasters()
    {
        if (masters is not null)
        {
            return;
        }

        byte[][] read = new byte[files.Length][];
        for (int n = 0; n < files.Length; n++)
        {
            read[n] = new byte[Block];
            ReadAt(n, 0, read[n]);
            ReadOnlySpan<byte> expected = n == 0 ? rootHash : read[n - 1].AsSpan(NextMasterAt);
            if (!HashEquals(read[n], expected))
            {
                throw new ImportException(DamagedMessage);
            }
        }

        masters = read;
    }

    /// <summary>Checks <paramref name="blocks"/> (whole data blocks from <paramref name="first"/> on) against their group's sub-table.</summary>
    private void CheckBlocks(int file, long group, int first, ReadOnlySpan<byte> blocks)
    {
        long key = file * (long)GroupsPerFile + group;
        if (key != subTableKey)
        {
            LoadMasters();
            subTableKey = -1;
            ReadAt(file, Block * (1 + group * (PerGroup + 1)), subTable);
            if (!HashEquals(subTable, masters![file].AsSpan((int)group * HashSize)))
            {
                throw new ImportException(DamagedMessage);
            }

            subTableKey = key;
        }

        for (int k = 0; k * Block < blocks.Length; k++)
        {
            if (!HashEquals(blocks.Slice(k * Block, Block), subTable.AsSpan((first + k) * HashSize)))
            {
                throw new ImportException(DamagedMessage);
            }
        }
    }
}
