using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// Which <c>default.xbe</c> the player has: only the two Conker discs the game was built from are
/// accepted (SPEC 9.3); anything else is described so the message can say what it is.
/// </summary>
public static class XbeCheck
{
    private const uint ConkerTitleId = 0x4D530051;

    private static readonly Dictionary<string, string> Supported = new(StringComparer.OrdinalIgnoreCase)
    {
        ["68A37F0D03D73DABC86A848DF5EC1820DD551254"] = "PAL",
        ["2232A1B3874D67B4BB4CF53A940420D2DAB59535"] = "USA",
    };

    /// <summary>The disc's region ("PAL", "USA"), or null when this XBE isn't supported.</summary>
    // SHA-1 only identifies the file here (the hashes in SPEC 9.3); nothing depends on it for security.
#pragma warning disable CA5350
    public static string? SupportedRegion(byte[] xbe) =>
        Supported.GetValueOrDefault(Convert.ToHexString(SHA1.HashData(xbe)));
#pragma warning restore CA5350

    /// <summary>Throws an <see cref="ImportException"/> saying what the XBE is, unless it is supported.</summary>
    public static string Require(byte[] xbe)
    {
        if (SupportedRegion(xbe) is { } region)
        {
            return region;
        }

        if (Describe(xbe) is not { } info)
        {
            throw new ImportException("This default.xbe isn't an Xbox program; the files may be damaged.");
        }

        string what = $"{info.Title} ({info.Region}, version {info.Version:X8})";
        throw new ImportException(info.TitleId == ConkerTitleId
            ? $"This is {what}, a version of Conker: Live & Reloaded that isn't supported. Only the PAL and USA retail discs work."
            : $"This is {what}, not Conker: Live & Reloaded.");
    }

    private sealed record Info(uint TitleId, string Title, string Region, uint Version);

    // The XBE header: "XBEH", base address at 0x104, certificate address at 0x118; in the
    // certificate the title ID at 0x08, the title name (40 UTF-16 chars) at 0x0C, the game
    // regions at 0xA0 and the version at 0xAC.
    private static Info? Describe(byte[] xbe)
    {
        if (xbe.Length < 0x200 || Encoding.ASCII.GetString(xbe, 0, 4) != "XBEH")
        {
            return null;
        }

        uint baseAddress = BinaryPrimitives.ReadUInt32LittleEndian(xbe.AsSpan(0x104));
        long cert = (long)BinaryPrimitives.ReadUInt32LittleEndian(xbe.AsSpan(0x118)) - baseAddress;
        if (cert < 0 || cert + 0xB0 > xbe.Length)
        {
            return null;
        }

        ReadOnlySpan<byte> c = xbe.AsSpan((int)cert);
        string title = Encoding.Unicode.GetString(c.Slice(0x0C, 80)).TrimEnd('\0');
        uint regions = BinaryPrimitives.ReadUInt32LittleEndian(c[0xA0..]);
        List<string> names = [];
        if ((regions & 1) != 0)
        {
            names.Add("North America");
        }

        if ((regions & 2) != 0)
        {
            names.Add("Japan");
        }

        if ((regions & 4) != 0)
        {
            names.Add("rest of the world");
        }

        return new Info(
            BinaryPrimitives.ReadUInt32LittleEndian(c[0x08..]),
            title.Length > 0 ? title : "an unnamed Xbox game",
            names.Count > 0 ? string.Join(", ", names) : "no region",
            BinaryPrimitives.ReadUInt32LittleEndian(c[0xAC..]));
    }
}
