namespace ConkerLauncher.Services;

/// <summary>
/// Copies the player's Conker disc into <c>game\</c> (SPEC 9.3): a full disc image or an XISO,
/// a folder (picked by its <c>default.xbe</c>), a .zip/.7z/.rar holding either, or the Xbox
/// Store version's package (picked by its <c>DefaultPackage</c> or a <c>Data####</c> file).
/// </summary>
/// <remarks>
/// Everything is written to <c>game.import\</c> first and swapped in at the end, so a cancelled
/// or failed import leaves the old files as they were. A disc image inside an archive is
/// unpacked to <c>import.tmp\</c> first and deleted afterwards. Both live next to <c>game\</c>
/// (same drive, so the swap is a rename); leftovers from a crash are removed at start-up.
/// </remarks>
public sealed class GameImporter(GameFolder folder)
{
    private string Staging => Path.Combine(folder.Root, "game.import");

    private string TempDir => Path.Combine(folder.Root, "import.tmp");

    /// <summary>The types Import Files takes; <see cref="StorePackage.FileName"/> = the Xbox Store version's package.</summary>
    public static readonly string[] Extensions = [".iso", ".xiso", ".zip", ".7z", ".rar", ".xbe", StorePackage.FileName];

    /// <summary>Removes what an interrupted import left behind.</summary>
    public void CleanLeftovers()
    {
        ImportTools.RemoveFolder(Staging);
        ImportTools.RemoveFolder(TempDir);
    }

    /// <summary>The words the Files tab shows for an import of the Xbox Store version.</summary>
    public const string StoreVersion = "Xbox Store version";

    /// <summary>Imports <paramref name="source"/>; returns what was imported ("PAL disc", "USA disc", <see cref="StoreVersion"/>).</summary>
    public string Import(string source, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        CleanLeftovers();
        try
        {
            string what = StorePackage.IsPackageName(source)
                ? FromPackage(source, progress, cancel)
                : (Path.GetExtension(source).ToLowerInvariant() switch
                {
                    ".xbe" => FromFolder(Path.GetDirectoryName(Path.GetFullPath(source))!, progress, cancel),
                    ".zip" or ".7z" or ".rar" => FromArchive(source, progress, cancel),
                    _ => FromDisc(source, progress, cancel),
                }) + " disc";
            progress.Report(new JobProgress("Finishing", null));
            ImportTools.Swap(Staging, folder.GameDir);
            return what;
        }
        finally
        {
            CleanLeftovers();
        }
    }

    private string FromDisc(string image, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        progress.Report(new JobProgress("Reading the disc image", null));
        using XdvdfsImage disc = XdvdfsImage.Open(image)
            ?? throw new ImportException($"{Path.GetFileName(image)} isn't an Xbox disc image.");
        List<XdvdfsEntry> entries = disc.ListAll();
        XdvdfsEntry xbe = entries.FirstOrDefault(e => !e.IsDirectory && e.Path.Equals("default.xbe", StringComparison.OrdinalIgnoreCase))
            ?? throw new ImportException("The disc image has no default.xbe: it isn't an Xbox game disc.");
        string region = XbeCheck.Require(disc.ReadAll(xbe));
        CopyEntries(disc, entries, "The disc image", progress, cancel);
        return region;
    }

    /// <summary>
    /// The Xbox Store version (to-do 37): the quick check of the package first (its files all
    /// there, the hash chain, three sampled groups), then every block hash-checked as the copy
    /// reads it. Any failure stops with one of the two approved messages and leaves <c>game\</c> as it was.
    /// </summary>
    private string FromPackage(string pick, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        progress.Report(new JobProgress("Reading the Xbox Store version", null));
        string header = StorePackage.HeaderFor(pick);
        if (!File.Exists(header))
        {
            throw new ImportException(GodPackageStream.PartMessage);
        }

        GodPackageStream package = GodPackageStream.Open(header);
        try
        {
            package.CheckSample();
            package.VerifyReads = true;
        }
        catch
        {
            package.Dispose();
            throw;
        }

        using XdvdfsImage image = XdvdfsImage.OpenPackage(package);
        List<XdvdfsEntry> entries = image.ListAll();
        XdvdfsEntry xbe = entries.FirstOrDefault(e => !e.IsDirectory && e.Path.Equals("default.xbe", StringComparison.OrdinalIgnoreCase))
            ?? throw new ImportException(GodPackageStream.DamagedMessage);
        string region = XbeCheck.Require(image.ReadAll(xbe));
        CopyEntries(image, entries, "The package", progress, cancel);

        // The copy as written: default.xbe still the same build.
        if (XbeCheck.Require(File.ReadAllBytes(Path.Combine(Staging, "default.xbe"))) != region)
        {
            throw new ImportException(GodPackageStream.DamagedMessage);
        }

        return StoreVersion;
    }

    /// <summary>Copies every entry of <paramref name="source"/> into the staging folder, after the free space check.</summary>
    private void CopyEntries(XdvdfsImage source, List<XdvdfsEntry> entries, string sourceName, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        long total = entries.Sum(e => e.Size);
        ImportTools.RequireSpace(folder.Root, total);
        Directory.CreateDirectory(Staging);
        ByteProgress bytes = new(progress, "Copying the game files", total);
        foreach (XdvdfsEntry entry in entries)
        {
            cancel.ThrowIfCancellationRequested();
            string path = ImportTools.SafePath(Staging, entry.Path)
                ?? throw new ImportException($"{sourceName} has a bad file name ({entry.Path}).");
            if (entry.IsDirectory)
            {
                Directory.CreateDirectory(path);
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using FileStream output = new(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
            source.Copy(entry, output, bytes.Add, cancel);
        }
    }

    private string FromFolder(string source, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        string full = Path.GetFullPath(source).TrimEnd('\\') + "\\";
        string own = Path.GetFullPath(folder.GameDir).TrimEnd('\\') + "\\";
        if (full.StartsWith(own, StringComparison.OrdinalIgnoreCase))
        {
            throw new ImportException("These are the imported files already. Pick the disc image or the folder you imported from.");
        }

        string region = XbeCheck.Require(File.ReadAllBytes(Path.Combine(source, "default.xbe")));
        List<FileInfo> files = [.. new DirectoryInfo(source).EnumerateFiles("*", SearchOption.AllDirectories)];
        long total = files.Sum(f => f.Length);
        ImportTools.RequireSpace(folder.Root, total);
        Directory.CreateDirectory(Staging);
        ByteProgress bytes = new(progress, "Copying the game files", total);
        foreach (FileInfo file in files)
        {
            cancel.ThrowIfCancellationRequested();
            string path = Path.Combine(Staging, Path.GetRelativePath(source, file.FullName));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using FileStream input = new(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            using FileStream output = new(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
            ImportTools.CopyStream(input, output, bytes, cancel);
        }

        return region;
    }

    private string FromArchive(string source, IProgress<JobProgress> progress, CancellationToken cancel)
    {
        progress.Report(new JobProgress("Opening the archive", null));
        using var archive = ImportTools.OpenArchive(source);
        List<(string Key, long Size)> files = ImportTools.ListFiles(archive);

        // A disc image inside: unpack it, then import it like any other image.
        if (files.FirstOrDefault(f => IsImage(f.Key)) is { Key: not null } image)
        {
            // Room for the unpacked image and the game files copied from it (at most its size).
            ImportTools.RequireSpace(folder.Root, image.Size * 2);
            string name = Path.GetFileName(image.Key);
            ImportTools.Extract(archive, TempDir, key => key == image.Key ? name : null,
                new ByteProgress(progress, "Unpacking the disc image", image.Size), cancel);
            return FromDisc(Path.Combine(TempDir, name), progress, cancel);
        }

        // A game folder inside: the files next to the shallowest default.xbe.
        (string Key, long Size) xbe = files
            .Where(f => Path.GetFileName(f.Key).Equals("default.xbe", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f.Key.Count(c => c == '/'))
            .FirstOrDefault();
        if (xbe.Key is null)
        {
            throw new ImportException($"{Path.GetFileName(source)} holds no disc image (.iso) and no default.xbe.");
        }

        string prefix = xbe.Key[..^"default.xbe".Length];
        ImportTools.RequireSpace(folder.Root, files.Where(f => f.Key.StartsWith(prefix, StringComparison.Ordinal)).Sum(f => f.Size));
        ImportTools.Extract(archive, Staging, key => key.StartsWith(prefix, StringComparison.Ordinal) ? key[prefix.Length..] : null,
            new ByteProgress(progress, "Unpacking the game files", files.Where(f => f.Key.StartsWith(prefix, StringComparison.Ordinal)).Sum(f => f.Size)),
            cancel);
        return XbeCheck.Require(File.ReadAllBytes(Path.Combine(Staging, "default.xbe")));
    }

    private static bool IsImage(string key) =>
        key.EndsWith(".iso", StringComparison.OrdinalIgnoreCase) || key.EndsWith(".xiso", StringComparison.OrdinalIgnoreCase);
}
