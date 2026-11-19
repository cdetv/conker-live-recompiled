namespace ConkerLauncher.Services;

/// <summary>
/// Finds the Xbox Store version's package (SPEC 9.3) from what the player picked or dropped:
/// its header file <c>DefaultPackage</c>, a <c>Data####</c> file inside <c>DefaultPackage.data</c>,
/// or a folder holding the package: <c>DefaultPackage.data</c>, <c>Content\Game</c>,
/// <c>Content</c> or the install folder (<c>C:\XboxGames\Conker- Live and Reloaded</c> on the
/// user's PC); and finds an installed copy by itself (<see cref="FindInstalled()"/>, the Import
/// Xbox Store version button). The package itself is read by <see cref="GodPackageStream"/>.
/// </summary>
public static class StorePackage
{
    /// <summary>The header file's name; also the file browser's marker for "takes the Store package" in a choice's types.</summary>
    public const string FileName = GodPackageStream.PackageName;

    /// <summary>Conker: Live &amp; Reloaded's title id (in the package header).</summary>
    public const uint ConkerTitleId = 0x4D530051;

    private const string DataFolder = FileName + ".data";
    private const string DefaultLibrary = "XboxGames";
    private const string GamingRoot = ".GamingRoot";

    /// <summary>
    /// The installed Xbox Store versions of Conker on this PC's ready fixed drives: their
    /// <c>DefaultPackage</c> paths (usually none or one). Reads only folder lists and package
    /// headers; a drive or folder that can't be read is skipped. Slow drives may wake up: run it
    /// off the UI thread.
    /// </summary>
    public static List<string> FindInstalled() => FindInstalled(FixedDrives());

    /// <summary>
    /// As <see cref="FindInstalled()"/> on these drive tops. Each drive's Xbox app library folders:
    /// those its <c>.GamingRoot</c> names ("RGBX", a count, then UTF-16 folder names, each ended
    /// by a 0; "XboxGames" on C: here, "Xbox Games" on E:), and <c>XboxGames</c>. In each, every
    /// game folder's <c>Content\Game\DefaultPackage</c> with a package header of Conker's title id.
    /// </summary>
    public static List<string> FindInstalled(IEnumerable<string> driveRoots)
    {
        List<string> found = [];
        foreach (string root in driveRoots)
        {
            foreach (string library in LibraryFolders(root))
            {
                IEnumerable<string> games;
                try
                {
                    games = Directory.Exists(library) ? [.. Directory.EnumerateDirectories(library)] : [];
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
                {
                    continue;
                }

                foreach (string game in games)
                {
                    string header = Path.Combine(game, "Content", "Game", FileName);
                    if (GodPackageStream.ReadHeader(header) is { } bytes
                        && System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(0x360)) == ConkerTitleId
                        && !found.Contains(header, StringComparer.OrdinalIgnoreCase))
                    {
                        found.Add(header);
                    }
                }
            }
        }

        return found;
    }

    /// <summary>The library folder names in a <c>.GamingRoot</c> file's bytes (none when it isn't one).</summary>
    public static List<string> ReadGamingRoot(ReadOnlySpan<byte> file)
    {
        List<string> names = [];
        if (file.Length < 8 || !file[..4].SequenceEqual("RGBX"u8))
        {
            return names;
        }

        uint count = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(file[4..]);
        int at = 8;
        for (uint n = 0; n < count && n < 64 && at + 2 <= file.Length; n++)
        {
            int end = at;
            while (end + 2 <= file.Length && (file[end] != 0 || file[end + 1] != 0))
            {
                end += 2;
            }

            string name = System.Text.Encoding.Unicode.GetString(file[at..end]);
            if (name.Length > 0 && name.IndexOfAny(Path.GetInvalidPathChars()) < 0)
            {
                names.Add(name);
            }

            at = end + 2;
        }

        return names;
    }

    private static List<string> LibraryFolders(string root)
    {
        List<string> folders = [];
        try
        {
            string file = Path.Combine(root, GamingRoot);
            if (File.Exists(file) && new FileInfo(file).Length < 64 * 1024)
            {
                folders.AddRange(ReadGamingRoot(File.ReadAllBytes(file)).Select(name => Path.Combine(root, name.TrimStart('\\'))));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException)
        {
            // An unreadable .GamingRoot: the default folder still counts.
        }

        string standard = Path.Combine(root, DefaultLibrary);
        if (!folders.Contains(standard, StringComparer.OrdinalIgnoreCase))
        {
            folders.Add(standard);
        }

        return folders;
    }

    private static List<string> FixedDrives()
    {
        List<string> roots = [];
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            try
            {
                if (drive.DriveType == DriveType.Fixed && drive.IsReady)
                {
                    roots.Add(drive.RootDirectory.FullName);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
            {
                // A drive that went away while asked: left out.
            }
        }

        return roots;
    }

    /// <summary>True when <paramref name="types"/> (a file choice's extensions) take the Store package.</summary>
    public static bool Takes(IReadOnlyList<string> types) =>
        types.Any(t => t.TrimStart('*').Equals(FileName, StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// A file the browser lists as the package, by its name only (nothing is read): <c>DefaultPackage</c>,
    /// or <c>Data####</c> in a folder named <c>DefaultPackage.data</c>.
    /// </summary>
    public static bool IsPackageName(string path)
    {
        string name = Path.GetFileName(path);
        return name.Equals(FileName, StringComparison.OrdinalIgnoreCase)
            || (IsDataName(name) && Path.GetFileName(Path.GetDirectoryName(path)) is { } parent
                && parent.Equals(DataFolder, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// The package's header file that <paramref name="path"/> leads to, when it has a package
    /// header ("PIRS", an original Xbox game); null otherwise.
    /// </summary>
    public static string? Find(string path) => Candidates(path).FirstOrDefault(c => GodPackageStream.ReadHeader(c) is not null);

    /// <summary>
    /// The header file a pick of the package by name stands for (<c>DefaultPackage</c> itself,
    /// or the one next to a <c>Data####</c> file's folder), whether or not it is there.
    /// </summary>
    public static string HeaderFor(string path)
    {
        string full = Path.GetFullPath(path);
        return IsPackageName(full) && IsDataName(Path.GetFileName(full)) && Path.GetDirectoryName(Path.GetDirectoryName(full)) is { } game
            ? Path.Combine(game, FileName)
            : full;
    }

    private static IEnumerable<string> Candidates(string path)
    {
        string full;
        try
        {
            full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException)
        {
            yield break;
        }

        if (File.Exists(full))
        {
            if (IsPackageName(full))
            {
                yield return HeaderFor(full);
            }

            yield break;
        }

        if (!Directory.Exists(full))
        {
            yield break;
        }

        if (Path.GetFileName(full).Equals(DataFolder, StringComparison.OrdinalIgnoreCase)
            && Path.GetDirectoryName(full) is { } game)
        {
            yield return Path.Combine(game, FileName);
        }

        yield return Path.Combine(full, FileName);
        yield return Path.Combine(full, "Game", FileName);
        yield return Path.Combine(full, "Content", "Game", FileName);
    }

    private static bool IsDataName(string name) =>
        name.Length == 8 && name.StartsWith("Data", StringComparison.OrdinalIgnoreCase) && name.AsSpan(4).IndexOfAnyExceptInRange('0', '9') < 0;
}
