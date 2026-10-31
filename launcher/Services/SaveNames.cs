using System.Globalization;
using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// Save folders named after their saves (SPEC 9.1, as <c>src/save_names.c</c> in the game):
/// <c>saves\Game1</c>, <c>saves\drop</c> instead of XAPI's <c>UserData\4d530051\&lt;12 hex&gt;</c>,
/// whose hex is a hash of the save's name. The game translates between the two; the launcher
/// converts the old layout and matches saves by that hash.
/// </summary>
public static class SaveNames
{
    public const string SavesFolder = "saves";

    // The old layout: XAPI's U:\ (conker-notes "Saves").
    public const string OldSlotsPath = @"UserData\4d530051";

    private static readonly string[] SlotWords = ["Game ", "Spiel ", "Jeu ", "Juego ", "Partita "];

    private static readonly string[] Reserved = ["CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"];

    /// <summary>XAPI's folder name for a save name (0x0042E6BB): the UTF-16 characters as one number modulo 2^48 - 59.</summary>
    public static string Hash(string name)
    {
        ulong h = 0;
        foreach (char c in name)
        {
            h = ((h * 0x10000) + c) % 0xFFFFFFFFFFC5UL;
        }

        return h.ToString("X12", CultureInfo.InvariantCulture);
    }

    /// <summary>The folder a save name gets: GameN for the story slots in every language, else the name made safe.</summary>
    public static string FolderFor(string name)
    {
        foreach (string word in SlotWords)
        {
            if (name.Length == word.Length + 1 && name.StartsWith(word, StringComparison.Ordinal) && name[^1] is >= '1' and <= '9')
            {
                return "Game" + name[^1];
            }
        }

        var safe = new StringBuilder(name.Length);
        foreach (char c in name)
        {
            safe.Append(c < 32 || "<>:\"/\\|?*".Contains(c) ? '_' : c);
        }

        string folder = safe.ToString().TrimEnd(' ', '.');
        if (folder.Length == 0)
        {
            folder = "_";
        }

        return Reserved.Contains(folder, StringComparer.OrdinalIgnoreCase) ? folder + "_" : folder;
    }

    /// <summary>The save's name from <c>SaveMeta.xbx</c> (UTF-16, <c>Name=Game 1</c>), or null.</summary>
    public static string? MetaName(string dir)
    {
        try
        {
            string meta = Path.Combine(dir, "SaveMeta.xbx");
            if (File.Exists(meta))
            {
                foreach (string line in File.ReadAllText(meta, Encoding.Unicode).Split('\n'))
                {
                    string l = line.Trim('\r', '\uFEFF', ' ');
                    if (l.StartsWith("Name=", StringComparison.Ordinal) && l.Length > 5)
                    {
                        return l[5..];
                    }
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }

        return null;
    }

    /// <summary>The hex a save folder stands for: its name's hash, else its own name when that is a hex name.</summary>
    public static string? HexOf(string dir) =>
        MetaName(dir) is { } name ? Hash(name)
        : IsHex12(Path.GetFileName(dir)) ? Path.GetFileName(dir).ToUpperInvariant()
        : null;

    /// <summary>The save folders in a user files folder, by hex.</summary>
    public static Dictionary<string, string> Folders(string root)
    {
        Dictionary<string, string> found = new(StringComparer.OrdinalIgnoreCase);
        string saves = Path.Combine(root, SavesFolder);
        if (Directory.Exists(saves))
        {
            foreach (string dir in Directory.EnumerateDirectories(saves))
            {
                if (HexOf(dir) is { } hex)
                {
                    found.TryAdd(hex, dir);
                }
            }
        }

        return found;
    }

    /// <summary><paramref name="name"/>, else "<paramref name="name"/> (2)", ...: a folder not in <c>saves\</c> yet.</summary>
    public static string FreeFolder(string savesDir, string name)
    {
        string free = name;
        for (int n = 2; Directory.Exists(Path.Combine(savesDir, free)); n++)
        {
            free = $"{name} ({n})";
        }

        return Path.Combine(savesDir, free);
    }

    /// <summary>
    /// Moves the old layout's save folders into <c>saves\</c> and renames a <c>saves\&lt;hex&gt;</c>
    /// folder that has a name, as the game does at its start. A save already in <c>saves\</c> keeps
    /// that copy; the old one goes to <c>backups\&lt;time&gt; old save layout\</c>.
    /// </summary>
    public static void Convert(string root)
    {
        string saves = Path.Combine(root, SavesFolder);
        string old = Path.Combine(root, OldSlotsPath);
        if (Directory.Exists(old))
        {
            foreach (string dir in Directory.EnumerateDirectories(old).Where(d => IsHex12(Path.GetFileName(d))).ToList())
            {
                string? name = MetaName(dir);
                string hex = name is null ? Path.GetFileName(dir).ToUpperInvariant() : Hash(name);
                Directory.CreateDirectory(saves);
                if (Folders(root).ContainsKey(hex))
                {
                    string stamp = DateTime.Now.ToString("yyyy-MM-dd HHmmss", CultureInfo.InvariantCulture);
                    string backup = Path.Combine(root, UserFiles.BackupsName, $"{stamp} old save layout");
                    Directory.CreateDirectory(backup);
                    Directory.Move(dir, Path.Combine(backup, Path.GetFileName(dir)));
                }
                else
                {
                    Directory.Move(dir, FreeFolder(saves, name is null ? Path.GetFileName(dir) : FolderFor(name)));
                }
            }
        }

        if (Directory.Exists(saves))
        {
            foreach (string dir in Directory.EnumerateDirectories(saves).Where(d => IsHex12(Path.GetFileName(d))).ToList())
            {
                if (MetaName(dir) is { } name)
                {
                    Directory.Move(dir, FreeFolder(saves, FolderFor(name)));
                }
            }
        }
    }

    private static bool IsHex12(string s) => s.Length == 12 && s.All(Uri.IsHexDigit);
}
