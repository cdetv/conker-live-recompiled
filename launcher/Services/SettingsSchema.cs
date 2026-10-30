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
/// buttons and the Slots table. A key row's Button is the controller button it presses (shown
/// unless ButtonShown is false), Section the heading it is under, Scheme the single player scheme
/// it belongs to (null: both), ClashesAll that its key clashes with every row of its tab. A reset
/// button's Keys are what it puts back; a fixed row's FixedKeys its keys as text.
/// </summary>
public sealed record MenuEntry(
    string Type, string Label, string? Key, string? Tip, string? Needs,
    IReadOnlyList<string>? Values, IReadOnlyList<string>? Names,
    double Min, double Max, string? Neutral, int Decimals, string? Unit, string? ResetTip,
    string? Button = null, bool ButtonShown = true, string? Section = null, string? Scheme = null,
    bool ClashesAll = false, string? Question = null, IReadOnlyList<string>? Keys = null,
    string? FixedKeys = null, int Count = 0, string? KeyboardKey = null, string? KeyboardOnKey = null);

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
                           IReadOnlyList<MenuEntry> controls, IReadOnlyList<KeyName> keys)
    {
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

    public IReadOnlyDictionary<string, SettingOption> ByKey { get; }

    /// <summary>Key name to the label F3 shows.</summary>
    public IReadOnlyDictionary<string, string> KeyLabels { get; }

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
        };
        using Process process = Process.Start(start) ?? throw new IOException("conker.exe didn't start.");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        string json = await process.StandardOutput.ReadToEndAsync(timeout.Token);
        await process.WaitForExitAsync(timeout.Token);
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
        return new SettingsSchema(options, menu, controls, keys);
    }

    private static MenuEntry Entry(JsonElement m) => new(
        Str(m, "type")!, Str(m, "label")!, Str(m, "key"), Str(m, "tip"), Str(m, "needs"),
        List(m, "values"), List(m, "names"), Num(m, "min"), Num(m, "max"), Str(m, "neutral"),
        (int)Num(m, "decimals"), Str(m, "unit"), Str(m, "reset_tip"),
        Button: Str(m, "button"), ButtonShown: Bool(m, "button_shown") ?? true, Section: Str(m, "section"),
        Scheme: Str(m, "scheme"), ClashesAll: Bool(m, "clashes_all") ?? false, Question: Str(m, "question"),
        Keys: List(m, "keys"), FixedKeys: Str(m, "keys"), Count: (int)Num(m, "count"),
        KeyboardKey: Str(m, "keyboard_key"), KeyboardOnKey: Str(m, "keyboard_on_key"));

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
                // controls_parse_key: any case, empty is none; stored as the lower-case name.
                string v = value.Length == 0 ? "none" : value.ToLowerInvariant();
                return KeyLabels.ContainsKey(v) ? v : null;
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
