using System.Globalization;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>Where the player's files are kept (SPEC 9.1, user files).</summary>
public enum UserFilesPlace
{
    GameFolder,
    AppData,
}

/// <summary>What the player chose for a save slot that is in both places (SPEC 9.3).</summary>
public enum ConflictChoice
{
    /// <summary>The kept place's save goes to <c>backups\</c>, then the moving one replaces it.</summary>
    BackupAndTransfer,

    /// <summary>The moving save goes to <c>backups\</c> and is deleted where it was.</summary>
    BackupAndDelete,

    /// <summary>The moving save is deleted (after a confirmation).</summary>
    DeleteSource,
}

/// <summary>A save slot found in both places: <see cref="SourceDir"/> was being moved into <see cref="TargetDir"/>.</summary>
public sealed record SaveConflict(string Slot, string Name, UserFilesPlace From, string SourceDir, string TargetDir,
                                  DateTime SourceTime, DateTime TargetTime, string Backups);

/// <summary>What a move did: files moved, and the ones that couldn't be (in use).</summary>
public sealed record MoveResult(int Moved, int Failed, List<SaveConflict> Conflicts);

/// <summary>
/// The player's files (SPEC 9.1): saves, <c>settings.ini</c>, logs, photos and caches, in
/// <c>userdata\</c> next to the game, or in <c>%LOCALAPPDATA%\conker-lr-recomp</c> (older
/// versions, the launcher's AppData choice, or a game folder that can't be written). Files in
/// the other place are moved into the one in use, each deleted only once its copy is in place.
/// </summary>
public sealed class UserFiles(GameFolder folder)
{
    /// <summary>Next to the .exe: the player chose AppData. conker.exe reads it too (main.c).</summary>
    public const string MarkerName = "user_files_in_appdata.txt";

    public const string BackupsName = "backups";

    // saves\<name>\: one folder per save slot or multiplayer profile (SaveNames).
    private const string SlotsPath = SaveNames.SavesFolder;

    private const string MovingSuffix = ".moving";

    // Rebuilt by the game when missing: never asked about, never backed up.
    private static readonly string[] CacheFolders = ["Cache", "CacheUncensored", "shader_cache", "HddCache", "SystemData"];

    // Kept side by side under another name when both places have one (photos, logs, backups).
    private static readonly string[] KeepBothFolders = ["logs", "Photos", BackupsName];

    public string GameFolderPath => Path.Combine(folder.Root, "userdata");

    /// <summary><c>CONKER_LAUNCHER_APPDATA</c> (development only) stands in for the AppData folder.</summary>
    public static string AppDataPath =>
        Environment.GetEnvironmentVariable("CONKER_LAUNCHER_APPDATA") is { Length: > 0 } dir
            ? Path.GetFullPath(dir)
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "conker-lr-recomp");

    private string Marker => Path.Combine(folder.Root, MarkerName);

    /// <summary>False when <c>userdata\</c> can't be written (Program Files): AppData is used.</summary>
    public bool GameFolderWritable { get; private set; } = true;

    /// <summary>The player's choice (the marker file).</summary>
    public UserFilesPlace Chosen => File.Exists(Marker) ? UserFilesPlace.AppData : UserFilesPlace.GameFolder;

    /// <summary>Where the files are kept now.</summary>
    public UserFilesPlace Place => Chosen == UserFilesPlace.GameFolder && GameFolderWritable ? UserFilesPlace.GameFolder : UserFilesPlace.AppData;

    public string Active => PathOf(Place);

    public string PathOf(UserFilesPlace place) => place == UserFilesPlace.GameFolder ? GameFolderPath : AppDataPath;

    public static string NameOf(UserFilesPlace place) => place == UserFilesPlace.GameFolder ? "the game folder" : "AppData";

    /// <summary>Files in the place not in use, to move in (none when the game folder can't be written).</summary>
    public bool OtherPlaceHasFiles
    {
        get
        {
            UserFilesPlace other = Place == UserFilesPlace.GameFolder ? UserFilesPlace.AppData : UserFilesPlace.GameFolder;
            return GameFolderWritable && HasFiles(PathOf(other));
        }
    }

    /// <summary>Tries writing a file in <c>userdata\</c> (a folder's read-only flag says nothing).</summary>
    public void CheckWritable()
    {
        bool existed = Directory.Exists(GameFolderPath);
        try
        {
            Directory.CreateDirectory(GameFolderPath);
            string probe = Path.Combine(GameFolderPath, "write_test.tmp");
            File.WriteAllText(probe, "");
            File.Delete(probe);
            GameFolderWritable = true;
            if (!existed && Chosen == UserFilesPlace.AppData)
            {
                Directory.Delete(GameFolderPath);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            GameFolderWritable = false;
        }
    }

    /// <summary>Records the player's choice; the caller then moves the files.</summary>
    public void Choose(UserFilesPlace place)
    {
        if (place == UserFilesPlace.AppData)
        {
            File.WriteAllText(Marker,
                "conker-launcher: this game keeps its saves, settings, photos and logs in\r\n" +
                "%LOCALAPPDATA%\\conker-lr-recomp. Delete this file (or choose the game folder in the\r\n" +
                "launcher's Files tab) to keep them in the userdata folder here instead.\r\n");
        }
        else
        {
            File.Delete(Marker);
        }
    }

    /// <summary>Named save folders in the place in use (an old layout there is converted, as the game does).</summary>
    public void ConvertSaveNames()
    {
        if (Directory.Exists(Active))
        {
            SaveNames.Convert(Active);
        }
    }

    /// <summary>
    /// Moves everything from the place not in use into <see cref="Active"/>. A save slot that is
    /// in both places and differs is left where it was and returned for the player to decide.
    /// </summary>
    public MoveResult MoveIn(IProgress<JobProgress>? progress, CancellationToken cancel)
    {
        UserFilesPlace from = Place == UserFilesPlace.GameFolder ? UserFilesPlace.AppData : UserFilesPlace.GameFolder;
        string source = PathOf(from);
        string target = Active;
        ConvertSaveNames();
        if (!Directory.Exists(source))
        {
            return new MoveResult(0, 0, []);
        }

        // An older version's saves are in the old layout: named folders first, then matched by name.
        SaveNames.Convert(source);
        string stamp = DateTime.Now.ToString("yyyy-MM-dd HHmmss", CultureInfo.InvariantCulture);
        string backups = Path.Combine(target, BackupsName);
        string sourceBackup = Path.Combine(backups, $"{stamp} from {Label(from)}");
        List<SaveConflict> conflicts = [];
        List<(string From, string To)> moves = [];
        List<string> deletes = [];

        foreach (string cache in CacheFolders)
        {
            string dir = Path.Combine(source, cache);
            if (Directory.Exists(dir))
            {
                if (HasFiles(Path.Combine(target, cache)))
                {
                    deletes.Add(dir);
                }
                else
                {
                    moves.AddRange(FilesIn(dir).Select(f => (f, Path.Combine(target, Path.GetRelativePath(source, f)))));
                }
            }
        }

        // Rebuilt at every start, sparse: copied to another drive they would take ~5 GB.
        deletes.AddRange(Directory.EnumerateFiles(source, "Partition*.img"));

        // Saves are matched by the hash of their name (SaveNames), not by folder name.
        string slots = Path.Combine(source, SlotsPath);
        Dictionary<string, string> targetSaves = SaveNames.Folders(target);
        HashSet<string> taken = new(StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(slots))
        {
            foreach (string slot in Directory.EnumerateDirectories(slots))
            {
                if (!HasFiles(slot))
                {
                    continue;
                }

                string? hex = SaveNames.HexOf(slot);
                string other;
                if (hex is not null && targetSaves.TryGetValue(hex, out string? same))
                {
                    other = same;
                }
                else
                {
                    // A free folder in the target, also among the ones this move fills.
                    string name = Path.GetFileName(slot), free = name;
                    for (int n = 2; Directory.Exists(Path.Combine(target, SlotsPath, free)) || taken.Contains(free); n++)
                    {
                        free = $"{name} ({n})";
                    }

                    taken.Add(free);
                    other = Path.Combine(target, SlotsPath, free);
                }

                if (!HasFiles(other))
                {
                    moves.AddRange(FilesIn(slot).Select(f => (f, Path.Combine(other, Path.GetRelativePath(slot, f)))));
                }
                else if (SameFiles(slot, other))
                {
                    deletes.Add(slot);
                }
                else
                {
                    conflicts.Add(new SaveConflict(Path.GetFileName(slot), SlotName(slot, other), from, slot, other,
                                                   LastSaved(slot), LastSaved(other), backups));
                }
            }
        }

        foreach (string file in FilesIn(source))
        {
            string rel = Path.GetRelativePath(source, file);
            string first = rel.Split(Path.DirectorySeparatorChar)[0];
            bool isSlot = rel.StartsWith(SlotsPath + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                          && rel.Count(c => c == Path.DirectorySeparatorChar) >= 2;
            if (isSlot || CacheFolders.Contains(first, StringComparer.OrdinalIgnoreCase)
                || (first == rel && rel.StartsWith("Partition", StringComparison.OrdinalIgnoreCase) && rel.EndsWith(".img", StringComparison.OrdinalIgnoreCase))
                || rel.EndsWith(MovingSuffix, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            string to = Path.Combine(target, rel);
            if (!File.Exists(to))
            {
                moves.Add((file, to));
            }
            else if (SameBytes(file, to))
            {
                deletes.Add(file);
            }
            else if (KeepBothFolders.Contains(first, StringComparer.OrdinalIgnoreCase))
            {
                moves.Add((file, FreeName(to)));
            }
            else
            {
                // settings.ini and the like: the place in use keeps its own; this one is kept in backups\.
                moves.Add((file, Path.Combine(sourceBackup, rel)));
            }
        }

        long total = moves.Sum(m => new FileInfo(m.From).Length);
        ByteProgress? bytes = progress is null ? null : new ByteProgress(progress, $"Moving user files from {Label(from)}", total);
        int moved = 0, failed = 0;
        foreach ((string fromFile, string toFile) in moves)
        {
            cancel.ThrowIfCancellationRequested();
            long length = new FileInfo(fromFile).Length;
            if (TryMove(fromFile, toFile))
            {
                moved++;
            }
            else
            {
                failed++;
            }

            bytes?.Add(length);
        }

        foreach (string path in deletes)
        {
            if (!TryDelete(path))
            {
                failed++;
            }
        }

        RemoveEmptyFolders(source);
        return new MoveResult(moved, failed, conflicts);
    }

    /// <summary>Carries out the player's choice for one save slot.</summary>
    public static void Resolve(SaveConflict conflict, ConflictChoice choice)
    {
        string stamp = DateTime.Now.ToString("yyyy-MM-dd HHmmss", CultureInfo.InvariantCulture);
        UserFilesPlace to = conflict.From == UserFilesPlace.GameFolder ? UserFilesPlace.AppData : UserFilesPlace.GameFolder;
        string sourceRoot = Path.GetFullPath(Path.Combine(conflict.SourceDir, "..", ".."));
        switch (choice)
        {
            case ConflictChoice.BackupAndTransfer:
                CopyFolder(conflict.TargetDir, BackupPath(conflict, stamp, to));
                DeleteFolder(conflict.TargetDir);
                MoveFolder(conflict.SourceDir, conflict.TargetDir);
                break;
            case ConflictChoice.BackupAndDelete:
                CopyFolder(conflict.SourceDir, BackupPath(conflict, stamp, conflict.From));
                DeleteFolder(conflict.SourceDir);
                break;
            case ConflictChoice.DeleteSource:
                DeleteFolder(conflict.SourceDir);
                break;
        }

        RemoveEmptyFolders(sourceRoot);
    }

    public static bool HasFiles(string dir) =>
        Directory.Exists(dir) && Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any();

    // backups\<time> from <place>\saves\<name>\: copied back into a user files folder, it is that save again.
    private static string BackupPath(SaveConflict conflict, string stamp, UserFilesPlace from)
    {
        string dir = Path.Combine(conflict.Backups, $"{stamp} from {Label(from)}");
        for (int n = 2; Directory.Exists(Path.Combine(dir, SlotsPath, conflict.Slot)); n++)
        {
            dir = Path.Combine(conflict.Backups, $"{stamp} from {Label(from)} {n}");
        }

        return Path.Combine(dir, SlotsPath, conflict.Slot);
    }

    private static string Label(UserFilesPlace place) => place == UserFilesPlace.GameFolder ? "game folder" : "AppData";

    /// <summary>The slot's name from its <c>SaveMeta.xbx</c> (UTF-16, <c>Name=Game 1</c>), else its folder name.</summary>
    private static string SlotName(params string[] dirs) =>
        dirs.Select(SaveNames.MetaName).FirstOrDefault(n => n is not null) ?? Path.GetFileName(dirs[0]);

    private static DateTime LastSaved(string dir) =>
        new DirectoryInfo(dir).EnumerateFiles("*", SearchOption.AllDirectories).Select(f => f.LastWriteTime).DefaultIfEmpty().Max();

    private static List<string> FilesIn(string dir) => Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).ToList();

    private static bool SameFiles(string a, string b)
    {
        List<string> files = [.. FilesIn(a).Select(f => Path.GetRelativePath(a, f))];
        return files.Count == FilesIn(b).Count && files.All(f => SameBytes(Path.Combine(a, f), Path.Combine(b, f)));
    }

    private static bool SameBytes(string a, string b)
    {
        try
        {
            FileInfo fa = new(a), fb = new(b);
            if (!fb.Exists || fa.Length != fb.Length)
            {
                return false;
            }

            return File.ReadAllBytes(a).AsSpan().SequenceEqual(File.ReadAllBytes(b));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // "photo.png" taken: "photo (2).png".
    private static string FreeName(string path)
    {
        string dir = Path.GetDirectoryName(path)!, name = Path.GetFileNameWithoutExtension(path), ext = Path.GetExtension(path);
        for (int n = 2; ; n++)
        {
            string free = Path.Combine(dir, $"{name} ({n}){ext}");
            if (!File.Exists(free))
            {
                return free;
            }
        }
    }

    /// <summary>
    /// Copies to <c>&lt;name&gt;.moving</c>, renames it into place, then deletes the original: an
    /// interrupted move never leaves a half file under the real name, and the original stays.
    /// </summary>
    private static bool TryMove(string from, string to)
    {
        try
        {
            MoveFile(from, to);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static void MoveFile(string from, string to)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        string temp = to + MovingSuffix;
        File.Copy(from, temp, overwrite: true);
        File.SetLastWriteTimeUtc(temp, File.GetLastWriteTimeUtc(from));
        File.Move(temp, to);
        File.Delete(from);
    }

    private static void CopyFolder(string from, string to)
    {
        foreach (string file in FilesIn(from))
        {
            string dest = Path.Combine(to, Path.GetRelativePath(from, file));
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(file, dest);
            File.SetLastWriteTimeUtc(dest, File.GetLastWriteTimeUtc(file));
        }
    }

    private static void MoveFolder(string from, string to)
    {
        foreach (string file in FilesIn(from))
        {
            MoveFile(file, Path.Combine(to, Path.GetRelativePath(from, file)));
        }

        DeleteFolder(from);
    }

    private static void DeleteFolder(string dir)
    {
        if (Directory.Exists(dir))
        {
            ImportTools.RemoveFolder(dir);
        }
    }

    private static bool TryDelete(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                ImportTools.RemoveFolder(path);
            }
            else
            {
                File.Delete(path);
            }

            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>Deletes the empty folders left in <paramref name="root"/>, and it too when empty.</summary>
    private static void RemoveEmptyFolders(string root)
    {
        if (!Directory.Exists(root))
        {
            return;
        }

        try
        {
            foreach (string dir in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories)
                                            .OrderByDescending(d => d.Length).ToList())
            {
                if (!Directory.EnumerateFileSystemEntries(dir).Any())
                {
                    Directory.Delete(dir);
                }
            }

            if (!Directory.EnumerateFileSystemEntries(root).Any())
            {
                Directory.Delete(root);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Something is still in use; the next move tidies up.
        }
    }
}
