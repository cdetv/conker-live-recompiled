using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace ConkerLauncher.Services;

/// <summary>
/// One <c>settings.ini</c> option as <c>conker.exe --dump-settings</c> describes it. Comment: the
/// lines above it in a new file, when it has its own; Help: the comment it falls under, as one line.
/// </summary>
public sealed record SettingOption(
    string Section, string Key, string Kind, string Default, IReadOnlyList<string>? Choices,
    double Min, double Max, string? Comment, string Help);

/// <summary>
/// One entry of the F2 or F3 menu, in the menu's order: a section heading, checkbox, choice or
/// slider; F3 also has tabs, notes, radio buttons, key rows, fixed keys (Menus tab), reset
/// buttons and the Slots table. Parent: the checkbox the entry sits under (shown, indented, only
/// while it is on), or a choice with ParentValue (shown only while it has that value: the Pop-in
/// Fix's parts under Character & foliage detail "extended", to-do 219). A key row's Button is the controller button it presses (shown
/// unless ButtonShown is false), Section the heading it is under, Scheme the single player scheme
/// it belongs to (null: both), ClashesAll that its key clashes with every row of its tab. A reset
/// button's Keys are what it puts back; a fixed row's FixedKeys its keys as text. Slot: the F3
/// slot (1-4) whose Options window the entry is in (rumble, sticks, gyro); 0 = none.
/// </summary>
public sealed record MenuEntry(
    string Type, string Label, string? Key, string? Tip, string? Needs,
    IReadOnlyList<string>? Values, IReadOnlyList<string>? Names,
    double Min, double Max, string? Neutral, int Decimals, string? Unit, string? ResetTip,
    string? Button = null, bool ButtonShown = true, string? Section = null, string? Scheme = null,
    bool ClashesAll = false, string? Question = null, IReadOnlyList<string>? Keys = null,
    string? FixedKeys = null, int Count = 0, string? KeyboardKey = null, string? KeyboardOnKey = null,
    string? Parent = null, string? Mute = null, int Slot = 0, ChordsInfo? Chords = null,
    string? Pad = null, PadBindsInfo? PadBinds = null, string? ParentValue = null);

/// <summary>A controller button an action can have (to-do 39): its settings.ini name ("rt") and label ("Right trigger").</summary>
public sealed record PadChoice(string Name, string Label);

/// <summary>
/// A single player action's controller bind (to-do 39, <c>src/pad_binds.c</c>): its name
/// (settings.ini <c>pad_classic_&lt;name&gt;</c> / <c>pad_modern_&lt;name&gt;</c>), where it works (0 the
/// main list, else a section's place), whether it needs a button, the one action it may share a
/// button with on the main list (Crouch and Attack), and its label per scheme (null: not in it).
/// </summary>
public sealed record PadAction(string Name, int Place, bool Required, string Shares, string? Classic, string? Modern);

/// <summary>F3 Controls > Single player's Controller column: the buttons one can pick and the actions.</summary>
public sealed record PadBindsInfo(IReadOnlyList<PadChoice> Buttons, IReadOnlyList<PadAction> Actions);

/// <summary>A controller chord one can pick (to-do 39): its settings.ini name ("back+start") and label ("Back + Start").</summary>
public sealed record ChordChoice(string Name, string Label);

/// <summary>A group of chord functions (Universal, Photo Mode, Cheats / Debug) and its rule.</summary>
public sealed record ChordGroup(string Name, string Tip);

/// <summary>
/// A function a chord drives: its settings.ini key, label, tip, group (the index in
/// <see cref="ChordsInfo.Groups"/>; 0 = Universal) and whether it may be unbound.
/// </summary>
public sealed record ChordFunction(string Key, string Label, string Tip, int Group, bool Required);

/// <summary>
/// F3 Slots' Chords window (to-do 39, <c>src/chords.c</c>): the chords one can pick, the groups,
/// and the functions that show (with --debug: the debug ones; the cheat's once unlocked). Two
/// functions may share a chord only when neither is Universal and they are in different groups.
/// </summary>
public sealed record ChordsInfo(
    IReadOnlyList<ChordChoice> Chords, IReadOnlyList<ChordGroup> Groups, IReadOnlyList<ChordFunction> Functions);

/// <summary>A quality preset (to-do 202): its name and the option values it sets.</summary>
public sealed record QualityPreset(string Name, IReadOnlyDictionary<string, string> Values);

/// <summary>A key name settings.ini takes and what the F3 menu calls it ("space", "Space").</summary>
public sealed record KeyName(string Name, string Label);

/// <summary>
/// What the Settings and Controls tabs show (SPEC 9.2/9.3), read from <c>conker.exe
/// --dump-settings</c> so the tabs never drift from the game's F2 and F3 menus: every option, the
/// menus' entries and the key names.
/// </summary>
public sealed class SettingsSchema
{
    private SettingsSchema(IReadOnlyList<SettingOption> options, IReadOnlyList<MenuEntry> menu,
                           IReadOnlyList<MenuEntry> controls, IReadOnlyList<KeyName> keys,
                           IReadOnlyList<QualityPreset> presets, QualityPreset? firstStart)
    {
        Presets = presets;
        FirstStart = firstStart;
        Options = options;
        Menu = menu;
        Controls = controls;
        Keys = keys;
        ByKey = options.ToDictionary(o => o.Key, StringComparer.OrdinalIgnoreCase);
        KeyLabels = keys.ToDictionary(k => k.Name, k => k.Label, StringComparer.OrdinalIgnoreCase);
    }

    public IReadOnlyList<SettingOption> Options { get; }

    /// <summary>The F2 menu.</summary>
    public IReadOnlyList<MenuEntry> Menu { get; }

    /// <summary>The F3 menu, every tab in turn ("tab" entries between them).</summary>
    public IReadOnlyList<MenuEntry> Controls { get; }

    public IReadOnlyList<KeyName> Keys { get; }

    /// <summary>The quality presets (Low to Ultra; none from a game before 0.3.1).</summary>
    public IReadOnlyList<QualityPreset> Presets { get; }

    /// <summary>
    /// The preset a new settings.ini takes on this PC (to-do 203: Low on a weak PC); null = the
    /// defaults. The game decides it (<c>settings_first_start_preset</c>) and says so in the dump.
    /// </summary>
    public QualityPreset? FirstStart { get; }

    /// <summary>An option's value in a new settings.ini on this PC: the first-start preset's, else the default.</summary>
    public string NewFileValue(SettingOption option) =>
        FirstStart is not null && FirstStart.Values.TryGetValue(option.Key, out string? v) ? v : option.Default;

    public IReadOnlyDictionary<string, SettingOption> ByKey { get; }

    /// <summary>Key name to the label F3 shows.</summary>
    public IReadOnlyDictionary<string, string> KeyLabels { get; }

    /// <summary>Keys an action may have (CONTROLS_MAX_KEYS in <c>src/controls.h</c>; to-do 38).</summary>
    public const int MaxKeys = 3;

    /// <summary>A key list's keys ("space, shift+e" → space, shift+e; "none" → none).</summary>
    public static List<string> SplitKeys(string value) =>
        [.. value.Split(',').Select(k => k.Trim().ToLowerInvariant()).Where(k => k.Length > 0 && k != "none")];

    /// <summary>One key's parts: the hold key of a combo (or null), the key, and whether it keeps.</summary>
    public static (string? Hold, string Input, bool Keep) BindParts(string bind)
    {
        string b = bind.Trim().ToLowerInvariant();
        bool keep = b.EndsWith(" keep", StringComparison.Ordinal);
        if (keep)
        {
            b = b[..^5].TrimEnd();
        }

        int plus = b.IndexOf('+');
        return plus < 0 ? (null, b, keep) : (b[..plus].Trim(), b[(plus + 1)..].Trim(), keep);
    }

    /// <summary>The same key, held or not (controls.c <c>same_bind</c>).</summary>
    public static bool SameBind(string a, string b)
    {
        var (ah, ai, _) = BindParts(a);
        var (bh, bi, _) = BindParts(b);
        return ah == bh && ai == bi;
    }

    /// <summary>
    /// One key as settings.ini writes it ("e", "shift+e", "shift+c keep"), or null when the game
    /// wouldn't take it (controls.c <c>controls_bind_ok</c>: not two of the same, not the wheel as
    /// the hold key, not Alt+Tab or Alt+F4, keep only on a combo).
    /// </summary>
    public string? NormalizeBind(string bind)
    {
        var (hold, input, keep) = BindParts(bind);
        if (!KeyLabels.ContainsKey(input) || input == "none")
        {
            return null;
        }

        if (hold is null)
        {
            return keep ? null : input;
        }

        if (!KeyLabels.ContainsKey(hold) || hold == "none" || hold == input || hold.StartsWith("wheel", StringComparison.Ordinal)
            || (hold == "alt" && input is "tab" or "f4"))
        {
            return null;
        }

        return $"{hold}+{input}{(keep ? " keep" : "")}";
    }

    /// <summary>Runs <c>conker.exe --dump-settings</c> and reads what it prints.</summary>
    public static async Task<SettingsSchema> LoadAsync(GameFolder folder)
    {
        if (!File.Exists(folder.GameExe))
        {
            throw new IOException("conker.exe is missing from the launcher's folder.");
        }

        var start = new ProcessStartInfo(folder.GameExe, "--dump-settings")
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,    // the game prints UTF-8 (to-do 262)
        };
        using Process process = OwnProcesses.Start(start) ?? throw new IOException("conker.exe didn't start.");
        string json;
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            json = await process.StandardOutput.ReadToEndAsync(timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
        }
        finally
        {
            OwnProcesses.Forget(process);
        }

        if (process.ExitCode != 0)
        {
            throw new IOException($"conker.exe --dump-settings ended with {process.ExitCode}.");
        }

        return Parse(json);
    }

    public static SettingsSchema Parse(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;
        var options = root.GetProperty("options").EnumerateArray().Select(o => new SettingOption(
            Str(o, "section")!, Str(o, "key")!, Str(o, "kind")!, Str(o, "default")!, List(o, "choices"),
            Num(o, "min"), Num(o, "max"), Str(o, "comment"), Str(o, "help") ?? "")).ToList();
        var menu = root.GetProperty("menu").EnumerateArray().Select(Entry).ToList();
        var controls = root.GetProperty("controls").EnumerateArray().Select(Entry).ToList();
        var keys = root.GetProperty("keys").EnumerateArray()
            .Select(k => new KeyName(Str(k, "name")!, Str(k, "label")!)).ToList();
        var presets = root.TryGetProperty("presets", out JsonElement ps) && ps.ValueKind == JsonValueKind.Array
            ? ps.EnumerateArray().Select(p => new QualityPreset(
                Str(p, "name")!,
                p.GetProperty("values").EnumerateObject().ToDictionary(
                    v => v.Name, v => v.Value.GetString() ?? "", StringComparer.OrdinalIgnoreCase))).ToList()
            : [];
        QualityPreset? firstStart = root.TryGetProperty("first_start", out JsonElement fs)
                                    && fs.ValueKind == JsonValueKind.Object
                                    && Str(fs, "preset") is { } firstName
            ? presets.FirstOrDefault(p => p.Name == firstName)
            : null;
        return new SettingsSchema(options, menu, controls, keys, presets, firstStart);
    }

    private static MenuEntry Entry(JsonElement m) => new(
        Str(m, "type")!, Str(m, "label")!, Str(m, "key"), Str(m, "tip"), Str(m, "needs"),
        List(m, "values"), List(m, "names"), Num(m, "min"), Num(m, "max"), Str(m, "neutral"),
        (int)Num(m, "decimals"), Str(m, "unit"), Str(m, "reset_tip"),
        Button: Str(m, "button"), ButtonShown: Bool(m, "button_shown") ?? true, Section: Str(m, "section"),
        Scheme: Str(m, "scheme"), ClashesAll: Bool(m, "clashes_all") ?? false, Question: Str(m, "question"),
        Keys: List(m, "keys"), FixedKeys: Str(m, "keys"), Count: (int)Num(m, "count"),
        KeyboardKey: Str(m, "keyboard_key"), KeyboardOnKey: Str(m, "keyboard_on_key"), Parent: Str(m, "parent"),
        Mute: Str(m, "mute"), Slot: (int)Num(m, "slot"),
        Chords: Str(m, "type") == "chords" ? ParseChords(m) : null,
        Pad: Str(m, "pad"), PadBinds: Str(m, "type") == "pad_binds" ? ParsePadBinds(m) : null,
        ParentValue: Str(m, "parent_value"));

    private static PadBindsInfo ParsePadBinds(JsonElement m)
    {
        IEnumerable<JsonElement> Arr(string name) =>
            m.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray() : [];

        return new PadBindsInfo(
            [.. Arr("buttons").Select(b => new PadChoice(Str(b, "name") ?? "", Str(b, "label") ?? ""))],
            [.. Arr("actions").Select(a => new PadAction(Str(a, "name") ?? "", (int)Num(a, "place"),
                                                         Bool(a, "required") ?? false, Str(a, "shares") ?? "",
                                                         Str(a, "classic"), Str(a, "modern")))]);
    }

    private static ChordsInfo ParseChords(JsonElement m)
    {
        IEnumerable<JsonElement> Arr(string name) =>
            m.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray() : [];

        return new ChordsInfo(
            [.. Arr("chords").Select(c => new ChordChoice(Str(c, "name") ?? "", Str(c, "label") ?? ""))],
            [.. Arr("groups").Select(g => new ChordGroup(Str(g, "name") ?? "", Str(g, "tip") ?? ""))],
            [.. Arr("functions").Select(fn => new ChordFunction(Str(fn, "key") ?? "", Str(fn, "label") ?? "",
                                                                Str(fn, "tip") ?? "", (int)Num(fn, "group"),
                                                                Bool(fn, "required") ?? false))]);
    }

    /// <summary>
    /// A value as the game would store it, or null when the game would ignore it (and use the
    /// default): the same checks as <c>normalize</c> in <c>src/settings.c</c>.
    /// </summary>
    public string? Normalize(SettingOption option, string value)
    {
        value = value.Trim();
        switch (option.Kind)
        {
            case "key":
            {
                // controls_parse_keys: a list of up to three keys (to-do 38), any case, empty or
                // "none" is none; a key is a name, a combo "shift+e" or a held combo "shift+c keep";
                // repeats dropped (a combo held or not is one key), past the third ignored.
                var keys = new List<string>();
                foreach (string part in value.Split(','))
                {
                    string v = part.Trim().ToLowerInvariant();
                    if (v.Length == 0 || v == "none")
                    {
                        continue;
                    }

                    string? bind = NormalizeBind(v);
                    if (bind is null)
                    {
                        return null;
                    }

                    if (keys.Count < MaxKeys && !keys.Any(k => SameBind(k, bind)))
                    {
                        keys.Add(bind);
                    }
                }

                return keys.Count == 0 ? "none" : string.Join(", ", keys);
            }
            case "bool":
                return value.ToLowerInvariant() switch
                {
                    "1" or "on" or "true" or "yes" => "1",
                    "0" or "off" or "false" or "no" => "0",
                    _ => null,
                };
            case "choice":
                return option.Choices?.FirstOrDefault(c => c.Equals(value, StringComparison.OrdinalIgnoreCase));
            case "resolution":
            {
                string v = value.ToLowerInvariant();
                if (v is "auto" or "auto-4:3" or "native")
                {
                    return v;
                }

                string[] wh = v.Split('x');
                return wh.Length == 2 && uint.TryParse(wh[0], out _) && uint.TryParse(wh[1], out _) ? v : null;
            }
            case "number":
                return double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double d)
                       && d >= option.Min - 1e-6 && d <= option.Max + 1e-6
                    ? FormatNumber(d)
                    : null;
            default:
                return value;
        }
    }

    /// <summary>At most two decimals, no trailing zeros ("1", "0.85"), as the game writes numbers.</summary>
    public static string FormatNumber(double d)
    {
        string s = Math.Round(d, 2).ToString("0.##", CultureInfo.InvariantCulture);
        return s == "-0" ? "0" : s;
    }

    private static string? Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static bool? Bool(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? v.GetBoolean()
            : null;

    private static double Num(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;

    private static List<string>? List(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray().Select(x => x.GetString() ?? "").ToList()
            : null;
}
