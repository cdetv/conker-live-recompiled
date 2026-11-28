using System.Diagnostics;
using System.Globalization;
using System.Net.Http.Headers;
using System.Text;
using Microsoft.Win32;

namespace ConkerLauncher.Services;

/// <summary>Steam's folder and <c>steam.exe</c>, from the registry.</summary>
public sealed record SteamInstall(string Root, string Exe);

/// <summary>What Add to Steam did: a shortcut updated (or added), how many pictures, for which account.</summary>
public sealed record SteamResult(bool Updated, int Pictures, int PicturesWanted, string Account);

/// <summary>Add to Steam stopped; the message says why (for the Files tab, in the language shown).</summary>
public sealed class SteamException(Said text) : Exception
{
    public Said Text { get; } = text;

    public override string Message => Text.ToString();
}

/// <summary>
/// Add to Steam on Windows (to-do 285, SPEC 9.3): the launcher as a non-Steam game in the
/// player's Steam library, with the SteamGridDB artwork. Steam is closed meanwhile (it
/// rewrites its shortcut list when it exits) and started again. A shortcut to the same
/// launcher is updated, never doubled. Linux and the Steam Deck use <c>add-to-steam.sh</c>
/// from the release zip instead (the same file formats, plus Proton 11), so this is hidden
/// under Wine.
/// </summary>
public static class SteamShortcut
{
    /// <summary>The shortcut's name in Steam (as <c>add-to-steam.sh</c>).</summary>
    public const string AppName = "Conker: Live & Recompiled";

    /// <summary>
    /// The SteamGridDB pictures the user picked (to-do 285): the end of Steam's file name in
    /// <c>userdata\&lt;account&gt;\config\grid</c> (after the shortcut's app id) and the direct
    /// link. Downloaded when the player clicks, never shipped (README credits the artists).
    /// </summary>
    private static readonly (string File, string Url)[] Art =
    [
        ("p.png", "https://cdn2.steamgriddb.com/grid/b31f094b6e677231d81e5b2a4b8c8aca.png"),      // cover 600x900, PopASquatt
        (".png", "https://cdn2.steamgriddb.com/grid/156bc0f12ef530de66826ad4b82a98af.png"),       // wide 920x430, PopASquatt
        ("_hero.png", "https://cdn2.steamgriddb.com/hero/9965b21b3968b6abc41143236b035c8c.png"),  // background, Jiquita
        ("_logo.png", "https://cdn2.steamgriddb.com/logo/8e7ad86ada2d18c6d7b0f5655a642a45.png"),  // logo, PopASquatt
        ("_icon.png", "https://cdn2.steamgriddb.com/icon/f50d8aa7aa4204ac97b2ef3ed37476f6.png"),  // icon, EVE Online
    ];

    private const string IconFile = "_icon.png";

    private static readonly HttpClient Http = CreateClient();

    /// <summary>Real Windows: under Wine (Proton) the button is hidden and the Linux script does this.</summary>
    public static bool Supported => OperatingSystem.IsWindows() && !Program.OnWine();

    /// <summary>
    /// Steam on this PC (the registry's <c>SteamPath</c>, with <c>steam.exe</c> there), or null.
    /// <c>CONKER_LAUNCHER_STEAM_DIR</c> (development only): a Steam folder used instead (a copy of
    /// Steam's files for tests); Steam itself is then neither closed nor started.
    /// </summary>
    public static SteamInstall? Find()
    {
        if (Environment.GetEnvironmentVariable("CONKER_LAUNCHER_STEAM_DIR") is { Length: > 0 } testDir)
        {
            return new SteamInstall(Path.GetFullPath(testDir), Path.Combine(Path.GetFullPath(testDir), "no-steam.exe"));
        }

        if (!OperatingSystem.IsWindows())
        {
            return null;
        }

        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            if (key?.GetValue("SteamPath") is not string path || path.Length == 0)
            {
                return null;
            }

            string root = Path.GetFullPath(path);
            string exe = key.GetValue("SteamExe") is string e && e.Length > 0 ? Path.GetFullPath(e) : Path.Combine(root, "steam.exe");
            return File.Exists(exe) ? new SteamInstall(root, exe) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException)
        {
            return null;
        }
    }

    public static bool IsRunning()
    {
        Process[] steam = Process.GetProcessesByName("steam");
        foreach (Process p in steam)
        {
            p.Dispose();
        }

        return steam.Length > 0;
    }

    /// <summary>Adds or updates the shortcut to <paramref name="launcherExe"/>; Steam is closed meanwhile and started again.</summary>
    public static async Task<SteamResult> AddAsync(SteamInstall steam, string launcherExe, IProgress<Said> progress)
    {
        (string account, string accountName) = PickAccount(steam.Root);

        progress.Report(S("Downloading the artwork from SteamGridDB ..."));
        byte[]?[] pictures = await Task.WhenAll(Art.Select(a => DownloadAsync(a.Url)));

        if (File.Exists(steam.Exe) && IsRunning())
        {
            progress.Report(S("Closing Steam ..."));
            await CloseSteamAsync(steam);
        }

        try
        {
            progress.Report(S("Adding the shortcut ..."));
            bool updated = await Task.Run(() => Write(steam.Root, account, launcherExe, pictures));
            return new SteamResult(updated, pictures.Count(p => p is not null), Art.Length, accountName);
        }
        finally
        {
            StartSteam(steam);      // also after a failure: it was closed for this
        }
    }

    // ── Steam ───────────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// The account that gets the shortcut: the one signed in while Steam runs (the registry's
    /// <c>ActiveUser</c>), else the most recent in <c>loginusers.vdf</c> (MostRecent, then
    /// AutoLogin, then its timestamp; newer Steams write no MostRecent), else the only one.
    /// </summary>
    private static (string Id, string Name) PickAccount(string root)
    {
        string userdata = Path.Combine(root, "userdata");
        List<string> folders = Directory.Exists(userdata)
            ? [.. Directory.GetDirectories(userdata).Select(Path.GetFileName).OfType<string>()
                  .Where(n => n != "0" && n.Length > 0 && n.All(char.IsAsciiDigit))]
            : [];
        if (folders.Count == 0)
        {
            throw new SteamException(S("No Steam account has signed in on this PC yet: start Steam, sign in, then try again."));
        }

        Dictionary<string, Dictionary<string, string>> users = ReadLoginUsers(root);
        string Name(string id) =>
            users.TryGetValue(id, out Dictionary<string, string>? u) && (u.GetValueOrDefault("personaname") ?? u.GetValueOrDefault("accountname")) is { Length: > 0 } n
                ? n : id;

        if (OperatingSystem.IsWindows()
            && Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null) is int active
            && active != 0 && folders.Contains(((uint)active).ToString(CultureInfo.InvariantCulture)))
        {
            string id = ((uint)active).ToString(CultureInfo.InvariantCulture);
            return (id, Name(id));
        }

        string best = folders.MaxBy(id =>
        {
            Dictionary<string, string> u = users.GetValueOrDefault(id) ?? [];
            long stamp = long.TryParse(u.GetValueOrDefault("timestamp"), NumberStyles.Integer, CultureInfo.InvariantCulture, out long t) ? t : 0;
            return (u.GetValueOrDefault("mostrecent") == "1" ? 1L << 62 : 0) + (u.GetValueOrDefault("autologin") == "1" ? 1L << 61 : 0) + stamp;
        })!;
        return (best, Name(best));
    }

    /// <summary><c>config\loginusers.vdf</c>: account id (the userdata folder's name) → its fields, keys in lower case.</summary>
    private static Dictionary<string, Dictionary<string, string>> ReadLoginUsers(string root)
    {
        Dictionary<string, Dictionary<string, string>> users = [];
        try
        {
            List<TextVdf.Node> top = TextVdf.Parse(File.ReadAllText(Path.Combine(root, "config", "loginusers.vdf")));
            foreach (TextVdf.Node user in top.FirstOrDefault(n => n.Key.Equals("users", StringComparison.OrdinalIgnoreCase))?.Children ?? [])
            {
                if (user.Children is { } fields && ulong.TryParse(user.Key, NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong steamId))
                {
                    users[(steamId & 0xFFFFFFFF).ToString(CultureInfo.InvariantCulture)] = fields
                        .Where(f => f.Value is not null)
                        .GroupBy(f => f.Key.ToLowerInvariant())
                        .ToDictionary(g => g.Key, g => g.First().Value!);
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException)
        {
            // No names then: the folder number stands in.
        }

        return users;
    }

    private static async Task CloseSteamAsync(SteamInstall steam)
    {
        try
        {
            Process.Start(new ProcessStartInfo(steam.Exe, "-shutdown") { UseShellExecute = false, WorkingDirectory = steam.Root })?.Dispose();
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            throw new SteamException(S("Steam couldn't be closed ({0}): close it yourself, then try again.", e.Message));
        }

        Stopwatch waited = Stopwatch.StartNew();
        while (IsRunning())
        {
            if (waited.Elapsed > TimeSpan.FromSeconds(90))
            {
                throw new SteamException(S("Steam didn't close, so nothing was changed: close it yourself (Steam menu > Exit), then try again."));
            }

            await Task.Delay(500);
        }

        await Task.Delay(TimeSpan.FromSeconds(2));     // its last writes
    }

    private static void StartSteam(SteamInstall steam)
    {
        if (!File.Exists(steam.Exe))
        {
            return;     // CONKER_LAUNCHER_STEAM_DIR
        }

        try
        {
            Process.Start(new ProcessStartInfo(steam.Exe) { UseShellExecute = true, WorkingDirectory = steam.Root })?.Dispose();
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // The player starts it; the shortcut is there either way.
        }
    }

    private static async Task<byte[]?> DownloadAsync(string url)
    {
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(30));
            byte[] data = await Http.GetByteArrayAsync(new Uri(url), timeout.Token);
            return data.AsSpan().StartsWith((ReadOnlySpan<byte>)[0x89, (byte)'P', (byte)'N', (byte)'G']) ? data : null;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException)
        {
            return null;    // that picture is left out; Steam shows its own there
        }
    }

    private static HttpClient CreateClient()
    {
        HttpClient client = new();
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("conker-launcher", "1.0"));
        return client;
    }

    // ── The shortcut ────────────────────────────────────────────────────────────────────────

    /// <summary>Writes the shortcut (Steam closed) and the pictures; true when one to this launcher was there already.</summary>
    private static bool Write(string root, string account, string launcherExe, byte[]?[] pictures)
    {
        string config = Path.Combine(root, "userdata", account, "config");
        string file = Path.Combine(config, "shortcuts.vdf");
        byte[]? old = File.Exists(file) && new FileInfo(file).Length > 0 ? File.ReadAllBytes(file) : null;
        List<BinaryVdf.Field> top;
        try
        {
            top = old is null ? [] : BinaryVdf.Read(old);
            if (old is not null && !BinaryVdf.Write(top).AsSpan().SequenceEqual(old))
            {
                throw new FormatException("it wouldn't be written back the same");
            }
        }
        catch (FormatException e)
        {
            throw new SteamException(S("Steam's list of non-Steam games has a form the launcher doesn't know ({0}), so nothing was changed.", e.Message));
        }

        BinaryVdf.Field? list = top.Find(f => f.Kind == BinaryVdf.Block && f.Key.Equals("shortcuts", StringComparison.OrdinalIgnoreCase));
        if (list is null)
        {
            list = new BinaryVdf.Field(BinaryVdf.Block, "shortcuts", new List<BinaryVdf.Field>());
            top.Add(list);
        }

        List<BinaryVdf.Field> entries = list.Children;
        string exe = $"\"{launcherExe}\"";
        string startDir = $"\"{Path.GetDirectoryName(launcherExe)}{Path.DirectorySeparatorChar}\"";
        List<BinaryVdf.Field>? entry = entries
            .Where(e => e.Kind == BinaryVdf.Block)
            .Select(e => e.Children)
            .FirstOrDefault(e => BinaryVdf.Get(e, "exe") is { Kind: BinaryVdf.Text } f && SameFile(f.Text, launcherExe));
        bool updated = entry is not null;
        uint appId = entry is not null && BinaryVdf.Get(entry, "appid") is { Kind: BinaryVdf.Int } id && id.Int != 0
            ? unchecked((uint)id.Int)
            : Crc32.Compute(Encoding.UTF8.GetBytes(exe + AppName)) | 0x80000000;   // Steam's id for a non-Steam game

        string grid = Path.Combine(config, "grid");
        int iconIndex = Array.FindIndex(Art, a => a.File == IconFile);
        string? icon = pictures[iconIndex] is not null ? Path.Combine(grid, appId.ToString(CultureInfo.InvariantCulture) + IconFile) : null;
        int signedId = unchecked((int)appId);
        if (entry is null)
        {
            entry =
            [
                new(BinaryVdf.Int, "appid", signedId), new(BinaryVdf.Text, "AppName", AppName), new(BinaryVdf.Text, "Exe", exe),
                new(BinaryVdf.Text, "StartDir", startDir), new(BinaryVdf.Text, "icon", icon ?? ""), new(BinaryVdf.Text, "ShortcutPath", ""),
                new(BinaryVdf.Text, "LaunchOptions", ""), new(BinaryVdf.Int, "IsHidden", 0), new(BinaryVdf.Int, "AllowDesktopConfig", 1),
                new(BinaryVdf.Int, "AllowOverlay", 1), new(BinaryVdf.Int, "OpenVR", 0), new(BinaryVdf.Int, "Devkit", 0),
                new(BinaryVdf.Text, "DevkitGameID", ""), new(BinaryVdf.Int, "DevkitOverrideAppID", 0), new(BinaryVdf.Int, "LastPlayTime", 0),
                new(BinaryVdf.Text, "FlatpakAppID", ""), new(BinaryVdf.Block, "tags", new List<BinaryVdf.Field>()),
            ];
            int next = entries.Select(e => int.TryParse(e.Key, NumberStyles.Integer, CultureInfo.InvariantCulture, out int n) ? n : -1).DefaultIfEmpty(-1).Max() + 1;
            entries.Add(new BinaryVdf.Field(BinaryVdf.Block, next.ToString(CultureInfo.InvariantCulture), entry));
        }
        else
        {
            BinaryVdf.Set(entry, BinaryVdf.Int, "appid", signedId);
            BinaryVdf.Set(entry, BinaryVdf.Text, "AppName", AppName);
            BinaryVdf.Set(entry, BinaryVdf.Text, "Exe", exe);
            BinaryVdf.Set(entry, BinaryVdf.Text, "StartDir", startDir);
            if (icon is not null)
            {
                BinaryVdf.Set(entry, BinaryVdf.Text, "icon", icon);
            }
        }

        Directory.CreateDirectory(grid);
        if (old is not null)
        {
            File.Copy(file, file + ".conker-backup", overwrite: true);
        }

        WriteFile(file, BinaryVdf.Write(top));
        for (int i = 0; i < Art.Length; i++)
        {
            if (pictures[i] is { } data)
            {
                WriteFile(Path.Combine(grid, appId.ToString(CultureInfo.InvariantCulture) + Art[i].File), data);
            }
        }

        return updated;
    }

    private static bool SameFile(string stored, string launcherExe)
    {
        string path = stored.Trim().Trim('"');
        try
        {
            return path.Length > 0 && string.Equals(Path.GetFullPath(path), Path.GetFullPath(launcherExe), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    private static void WriteFile(string path, byte[] data)
    {
        string temp = path + ".conker-new";
        File.WriteAllBytes(temp, data);
        File.Move(temp, path, overwrite: true);
    }
}

/// <summary>Steam's binary KeyValues (<c>shortcuts.vdf</c>): read and written back byte for byte.</summary>
internal static class BinaryVdf
{
    public const byte Block = 0, Text = 1, Int = 2, End = 8;

    /// <summary>One field: a block (<see cref="Children"/>), text, an int32 or other raw bytes.</summary>
    public sealed class Field(byte kind, string key, object value)
    {
        public byte Kind { get; set; } = kind;

        public string Key { get; } = key;

        public object Value { get; set; } = value;

        public List<Field> Children => (List<Field>)Value;

        public string Text => (string)Value;

        public int Int => (int)Value;
    }

    public static Field? Get(List<Field> fields, string key) =>
        fields.Find(f => f.Key.Equals(key, StringComparison.OrdinalIgnoreCase));

    /// <summary>Sets a field, keeping its key's spelling (older Steams wrote "exe", "appname"); a new one goes before the blocks.</summary>
    public static void Set(List<Field> fields, byte kind, string key, object value)
    {
        if (Get(fields, key) is { } field)
        {
            field.Kind = kind;
            field.Value = value;
            return;
        }

        int firstBlock = fields.FindIndex(f => f.Kind == Block);
        fields.Insert(firstBlock < 0 ? fields.Count : firstBlock, new Field(kind, key, value));
    }

    public static List<Field> Read(byte[] data)
    {
        int pos = 0;
        List<Field> fields = ReadBlock(data, ref pos);
        if (pos != data.Length)
        {
            throw new FormatException("bytes after the end");
        }

        return fields;
    }

    private static List<Field> ReadBlock(byte[] data, ref int pos)
    {
        List<Field> fields = [];
        while (true)
        {
            if (pos >= data.Length)
            {
                throw new FormatException("the file ends inside a block");
            }

            byte kind = data[pos++];
            if (kind == End)
            {
                return fields;
            }

            string key = ReadString(data, ref pos);
            object value = kind switch
            {
                Block => ReadBlock(data, ref pos),
                Text => ReadString(data, ref pos),
                Int => BitConverter.ToInt32(Take(data, ref pos, 4)),
                3 or 4 or 6 => Take(data, ref pos, 4),
                7 => Take(data, ref pos, 8),
                _ => throw new FormatException($"unknown field type {kind} at byte {pos - 1}"),
            };
            fields.Add(new Field(kind, key, value));
        }
    }

    private static byte[] Take(byte[] data, ref int pos, int count)
    {
        if (pos + count > data.Length)
        {
            throw new FormatException("the file ends inside a value");
        }

        byte[] bytes = data[pos..(pos + count)];
        pos += count;
        return bytes;
    }

    private static string ReadString(byte[] data, ref int pos)
    {
        int end = Array.IndexOf(data, (byte)0, pos);
        if (end < 0)
        {
            throw new FormatException("a text doesn't end");
        }

        string s = Encoding.UTF8.GetString(data, pos, end - pos);
        pos = end + 1;
        return s;
    }

    public static byte[] Write(List<Field> fields)
    {
        using MemoryStream stream = new();
        WriteBlock(stream, fields);
        return stream.ToArray();
    }

    private static void WriteBlock(MemoryStream stream, List<Field> fields)
    {
        foreach (Field f in fields)
        {
            stream.WriteByte(f.Kind);
            WriteString(stream, f.Key);
            switch (f.Value)
            {
                case List<Field> children:
                    WriteBlock(stream, children);
                    break;
                case string text:
                    WriteString(stream, text);
                    break;
                case int number:
                    stream.Write(BitConverter.GetBytes(number));
                    break;
                case byte[] raw:
                    stream.Write(raw);
                    break;
            }
        }

        stream.WriteByte(End);
    }

    private static void WriteString(MemoryStream stream, string s)
    {
        stream.Write(Encoding.UTF8.GetBytes(s));
        stream.WriteByte(0);
    }
}

/// <summary>Steam's text KeyValues (<c>loginusers.vdf</c>), read only: quoted keys and values, blocks.</summary>
internal static class TextVdf
{
    public sealed record Node(string Key, string? Value, List<Node>? Children);

    public static List<Node> Parse(string text)
    {
        int pos = 0;
        List<Node> nodes = ParseBlock(text, ref pos, top: true);
        return nodes;
    }

    private static List<Node> ParseBlock(string text, ref int pos, bool top)
    {
        List<Node> nodes = [];
        while (Next(text, ref pos) is { } token)
        {
            if (token == "}")
            {
                if (top)
                {
                    throw new FormatException("unexpected '}'");
                }

                return nodes;
            }

            string key = token;
            string value = Next(text, ref pos) ?? throw new FormatException("the file ends after a key");
            nodes.Add(value == "{"
                ? new Node(Unquote(key), null, ParseBlock(text, ref pos, top: false))
                : new Node(Unquote(key), Unquote(value), null));
        }

        return top ? nodes : throw new FormatException("a block isn't closed");
    }

    /// <summary>The next token: "{", "}" or a string (quotes kept, so a quoted "{" isn't a brace).</summary>
    private static string? Next(string text, ref int pos)
    {
        while (pos < text.Length)
        {
            char c = text[pos];
            if (char.IsWhiteSpace(c))
            {
                pos++;
            }
            else if (c == '/' && pos + 1 < text.Length && text[pos + 1] == '/')
            {
                int end = text.IndexOf('\n', pos);
                pos = end < 0 ? text.Length : end;
            }
            else if (c is '{' or '}')
            {
                pos++;
                return c.ToString();
            }
            else if (c == '"')
            {
                int start = pos++;
                while (pos < text.Length && text[pos] != '"')
                {
                    pos += text[pos] == '\\' ? 2 : 1;
                }

                pos = Math.Min(pos + 1, text.Length);
                return text[start..pos];
            }
            else
            {
                int start = pos;
                while (pos < text.Length && !char.IsWhiteSpace(text[pos]) && text[pos] is not ('{' or '}' or '"'))
                {
                    pos++;
                }

                return text[start..pos];
            }
        }

        return null;
    }

    private static string Unquote(string token) =>
        token.Length >= 2 && token[0] == '"' && token[^1] == '"' ? token[1..^1].Replace("\\\\", "\\", StringComparison.Ordinal) : token;
}

/// <summary>CRC-32 (IEEE, as zlib's): Steam's id for a non-Steam game is CRC-32(exe + name) | 0x80000000.</summary>
internal static class Crc32
{
    private static readonly uint[] Table = MakeTable();

    public static uint Compute(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc = Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        }

        return ~crc;
    }

    private static uint[] MakeTable()
    {
        uint[] table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            }

            table[i] = c;
        }

        return table;
    }
}
