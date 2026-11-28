using System.Text;

namespace ConkerLauncher.Services;

/// <summary>
/// The texture pack list (to-do 288 part 2d, SPEC 9.3 "Texture packs"): the folders in
/// <c>textures\packs</c> next to conker.exe, in settings.ini's <c>[texture_packs]</c> order, by the
/// same rules as the game's F2 window (<c>tex_packs_found</c> / <c>tex_packs_list</c> in
/// <c>src/tex_packs.c</c>), so both show one list.
/// </summary>
public static class TexturePacks
{
    /// <summary>The game's limit (MAX_PACKS in <c>src/tex_packs.c</c>).</summary>
    public const int Max = 64;

    /// <summary>The folder the packs are in.</summary>
    public static string Folder(GameFolder folder) => Path.Combine(TextureExport.TexturesFolder(folder), "packs");

    /// <summary>
    /// A name the game takes as a pack (<c>good_name</c>): at most 120 bytes, no control character,
    /// nothing Windows refuses in a folder name, no dot at its end.
    /// </summary>
    public static bool GoodName(string name) =>
        name.Length > 0 && Encoding.UTF8.GetByteCount(name) <= 120 && !name.EndsWith('.')
        && name.All(c => c >= ' ' && "\\/:*?\"<>|".IndexOf(c) < 0);

    /// <summary>As the game sorts new folders (<c>_stricmp</c>: ASCII letters lowered, then by code).</summary>
    private static int CompareNames(string a, string b) =>
        string.CompareOrdinal(Lower(a), Lower(b));

    private static string Lower(string s) =>
        string.Create(s.Length, s, (span, src) =>
        {
            for (int i = 0; i < src.Length; i++)
            {
                char c = src[i];
                span[i] = c is >= 'A' and <= 'Z' ? (char)(c + 32) : c;
            }
        });

    /// <summary>The pack folders there are, sorted by name; empty when there is no packs folder.</summary>
    public static List<string> Found(GameFolder folder)
    {
        var names = new List<string>();
        try
        {
            foreach (string dir in Directory.EnumerateDirectories(Folder(folder)))
            {
                string name = Path.GetFileName(dir);
                if (GoodName(name) && names.Count < Max)
                {
                    names.Add(name);
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // No packs folder yet (or it can't be read): no packs.
        }

        names.Sort(CompareNames);
        return names;
    }

    /// <summary>
    /// The list: settings.ini's lines for the folders that are there (the folder's own spelling),
    /// then the folders it doesn't name, off, by name. A line whose folder is gone is left out.
    /// </summary>
    public static List<(string Name, bool On)> List(IReadOnlyList<string> found, IReadOnlyList<(string Name, bool On)> saved)
    {
        var list = new List<(string Name, bool On)>();
        var used = new bool[found.Count];
        foreach ((string name, bool on) in saved)
        {
            int j = Index(found, name);
            if (j >= 0 && !used[j])
            {
                used[j] = true;
                list.Add((found[j], on));
            }
        }

        for (int j = 0; j < found.Count; j++)
        {
            if (!used[j])
            {
                list.Add((found[j], false));
            }
        }

        return list;
    }

    private static int Index(IReadOnlyList<string> found, string name)
    {
        for (int j = 0; j < found.Count; j++)
        {
            if (found[j].Equals(name, StringComparison.OrdinalIgnoreCase))
            {
                return j;
            }
        }

        return -1;
    }
}
