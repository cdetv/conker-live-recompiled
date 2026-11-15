using System.Collections.ObjectModel;
using Avalonia.Input;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// The Controls tab (SPEC 9.3): the F3 menu's tabs, built from <c>conker.exe --dump-settings</c>,
/// with the controllers from <c>--list-pads</c>. Writes settings.ini as the Settings tab does:
/// shortly after each change, only that option's line.
/// </summary>
public sealed partial class ControlsViewModel : ObservableObject, ISettingsOwner
{
    private readonly GameFolder folder;
    private readonly Dictionary<string, string> pending = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> values = new(StringComparer.OrdinalIgnoreCase);
    private readonly DispatcherTimer saveTimer;
    private readonly List<SettingItem> items = [];
    private SettingsSchema? schema;
    private SlotsItem? slots;
    private IReadOnlyList<Pad> pads = [];

    public ControlsViewModel(GameFolder folder)
    {
        this.folder = folder;
        saveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
        saveTimer.Tick += (_, _) => Flush();
    }

    /// <summary>What comes before F3's tabs ("Use keyboard and mouse").</summary>
    public ObservableCollection<SettingItem> TopItems { get; } = [];

    public ObservableCollection<MenuTab> Tabs { get; } = [];

    /// <summary>The game is running: its F3 menu rewrites settings.ini, so the tab is locked.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsEditable))]
    public partial bool IsLocked { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsEditable))]
    public partial bool IsLoaded { get; set; }

    public bool IsEditable => IsLoaded && !IsLocked;

    [ObservableProperty]
    public partial string? ErrorText { get; set; }

    /// <summary>The row whose key is being asked for ("Press a key"), or null.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsCapturing))]
    public partial KeyItem? Capturing { get; set; }

    public bool IsCapturing => Capturing is not null;

    /// <summary>Why the last key pressed for it wasn't taken.</summary>
    [ObservableProperty]
    public partial string? CaptureMessage { get; set; }

    /// <summary>Builds the tabs from what conker.exe described (error: why it couldn't).</summary>
    public void Load(SettingsSchema? loaded, string? error)
    {
        if (loaded is null)
        {
            ErrorText = $"The controls couldn't be read from conker.exe: {error}";
            return;
        }

        schema = loaded;
        TopItems.Clear();
        Tabs.Clear();
        items.Clear();
        MenuTab? tab = null;
        foreach (MenuEntry entry in schema.Controls)
        {
            if (entry.Type == "tab")
            {
                tab = new MenuTab(entry.Label);
                Tabs.Add(tab);
                continue;
            }

            SettingItem? item = entry.Type switch
            {
                "section" => new SectionItem(entry),
                "note" => new NoteItem(entry),
                "checkbox" => new CheckItem(this, entry),
                "choice" => new ChoiceItem(this, entry),
                "slider" => new SliderItem(this, entry),
                "radio" => new RadioItem(this, entry),
                "key" => new KeyItem(this, entry, Default(entry.Key!)),
                "fixed" => new FixedItem(entry),
                "reset" => new ResetItem(this, entry),
                "slots" => slots = new SlotsItem(this, entry),
                _ => null,
            };
            if (item is null)
            {
                continue;
            }

            if (item.Key is { } key && schema.ByKey.TryGetValue(key, out SettingOption? option) && item is not KeyItem)
            {
                item.Tip ??= option.Help;
            }

            items.Add(item);
            (tab?.Items ?? TopItems).Add(item);
        }

        IsLoaded = true;
        Reload();
    }

    /// <summary>Asks conker.exe which controllers are connected and names the Slots choices after them.</summary>
    [RelayCommand]
    public async Task RefreshPadsAsync()
    {
        if (!File.Exists(folder.GameExe))
        {
            return;
        }

        try
        {
            pads = await PadList.LoadAsync(folder);
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception
                                       or System.Text.Json.JsonException or OperationCanceledException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            ErrorText = $"The controllers couldn't be listed: {e.Message}";
            return;
        }

        slots?.Update(ValueOf, pads);
    }

    /// <summary>Shows what settings.ini holds now (after the game ran, F3 may have changed it).</summary>
    public void Reload()
    {
        if (schema is null)
        {
            return;
        }

        Flush();
        Dictionary<string, string> file;
        try
        {
            file = SettingsFile.Read(SaveFolder.SettingsIni);
        }
        catch (IOException e)
        {
            ErrorText = $"settings.ini couldn't be read: {e.Message}";
            return;
        }

        values.Clear();
        foreach (SettingOption option in schema.Options)
        {
            // A value the game would ignore shows as the default it uses instead.
            values[option.Key] = file.TryGetValue(option.Key, out string? v) && schema.Normalize(option, v) is { } ok
                ? ok
                : option.Default;
        }

        foreach (SettingItem item in items)
        {
            if (item.Key is { } key && values.TryGetValue(key, out string? value))
            {
                item.Show(value);
            }
        }

        slots?.Update(ValueOf, pads);
        UpdateKeys();
    }

    /// <summary>Writes the changes not saved yet (Play calls this before the game starts).</summary>
    public void Flush()
    {
        saveTimer.Stop();
        if (pending.Count == 0 || schema is null)
        {
            return;
        }

        try
        {
            SettingsFile.Write(SaveFolder.SettingsIni, schema, pending);
            ErrorText = null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            ErrorText = $"settings.ini couldn't be saved: {e.Message}";
        }

        pending.Clear();
    }

    public void Changed(string key, string value)
    {
        pending[key] = value;
        values[key] = value;
        saveTimer.Stop();
        saveTimer.Start();

        // The keyboard is both "Use keyboard and mouse" and a box in the Slots table; a key may
        // have a row in each scheme; the scheme picks which single player rows show. After the
        // control that changed is done.
        Dispatcher.UIThread.Post(() =>
        {
            foreach (SettingItem item in items.Where(i => i is CheckItem or KeyItem && i.Key == key))
            {
                item.Show(value);
            }

            if (slots is not null && slots.Keys.Contains(key, StringComparer.OrdinalIgnoreCase))
            {
                slots.Update(ValueOf, pads);
            }

            UpdateKeys();
        });
    }

    internal string ValueOf(string key) => values.GetValueOrDefault(key) ?? Default(key);

    /// <summary>A key list as F3 shows it: "Space / Shift + E", "-" for none.</summary>
    internal string KeyLabel(string value)
    {
        List<string> keys = SettingsSchema.SplitKeys(value);
        return keys.Count == 0
            ? schema?.KeyLabels.GetValueOrDefault("none") ?? "-"
            : string.Join(" / ", keys.Select(BindLabel));
    }

    /// <summary>One key as F3 shows it: "E", "Shift + E", "Shift + C (keep)".</summary>
    internal string BindLabel(string bind)
    {
        var (hold, input, keep) = SettingsSchema.BindParts(bind);
        string l = Name(input);
        if (hold is not null)
        {
            l = $"{Name(hold)} + {l}";
        }

        return keep ? l + " (keep)" : l;
    }

    private string Name(string key) => schema?.KeyLabels.GetValueOrDefault(key) ?? key;

    internal void StartCapture(KeyItem item)
    {
        CaptureMessage = null;
        Holding = null;
        Capturing = item;
    }

    internal void SetKey(KeyItem item, string name)
    {
        item.Value = name;
        Changed(item.Key!, name);
    }

    internal void ResetKeys(IEnumerable<string> keys)
    {
        foreach (string key in keys)
        {
            string value = Default(key);
            foreach (SettingItem item in items.Where(i => i.Key == key))
            {
                item.Show(value);
            }

            Changed(key, value);
        }
    }

    /// <summary>A key held while asking: alone when let go, a combo's hold key when another follows.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HoldingText))]
    public partial string? Holding { get; set; }

    public string? HoldingText => Holding is null ? null : $"Holding {Name(Holding)}: press another key for a combo, or let go";

    /// <summary>A key pressed while asking (the window sends every key): F3's rules for which are taken.</summary>
    public void TakeKey(Key key)
    {
        // Esc cancels, as in F3 (user 2026-11-14: Esc can't be bound).
        if (key == Key.Escape)
        {
            Holding = null;
            Capturing = null;
        }
        else if (KeyInput.IsGameOwn(key))
        {
            CaptureMessage = "F1-F3, F9 and F11 are the game's own keys";
        }
        else if (KeyInput.Name(key) is { } name)
        {
            Down(name);
        }
        else
        {
            CaptureMessage = $"That key can't be used ({key})";
        }
    }

    /// <summary>A key let go while asking.</summary>
    public void TakeKeyUp(Key key)
    {
        if (KeyInput.Name(key) is { } name)
        {
            Up(name);
        }
    }

    /// <summary>A mouse button pressed or let go on the question (not on its buttons).</summary>
    public void TakeMouse(MouseButton button)
    {
        if (KeyInput.Name(button) is { } name)
        {
            Down(name);
        }
    }

    public void TakeMouseUp(MouseButton button)
    {
        if (KeyInput.Name(button) is { } name)
        {
            Up(name);
        }
    }

    /// <summary>A wheel notch is taken at once: alone, or as the second key of a combo.</summary>
    public void TakeWheel(double delta)
    {
        if (delta != 0)
        {
            string wheel = delta > 0 ? "wheelup" : "wheeldown";
            string? hold = Holding;
            Holding = null;
            Take(hold is null ? wheel : $"{hold}+{wheel}");
        }
    }

    /// <summary>
    /// F3's recording (overlay.cpp <c>drain_events</c>): the first key down waits; let go alone it
    /// is that key; another pressed while it is held makes the combo.
    /// </summary>
    private void Down(string name)
    {
        if (Holding is null)
        {
            Holding = name;
            CaptureMessage = null;
        }
        else if (name != Holding)
        {
            string combo = $"{Holding}+{name}";
            Holding = null;
            if (schema?.NormalizeBind(combo) is null)
            {
                CaptureMessage = "Alt+Tab and Alt+F4 are Windows' own";
                return;
            }

            Take(combo);
        }
    }

    private void Up(string name)
    {
        if (name == Holding)
        {
            Holding = null;
            Take(name);
        }
    }

    [RelayCommand]
    private void NoKey()
    {
        if (Capturing is { } item)
        {
            Holding = null;
            Capturing = null;
            SetKey(item, "none");
        }
    }

    [RelayCommand]
    private void CancelCapture()
    {
        Holding = null;
        Capturing = null;
    }

    /// <summary>
    /// A key pressed for the row (to-do 38, F3's <c>ask_key</c>): one it has is taken off, another
    /// added; a fourth is refused and the question stays.
    /// </summary>
    private void Take(string bind)
    {
        if (Capturing is not { } item)
        {
            return;
        }

        List<string> keys = SettingsSchema.SplitKeys(item.Value);
        if (keys.RemoveAll(k => SettingsSchema.SameBind(k, bind)) > 0)
        {
            // taken off
        }
        else if (keys.Count >= SettingsSchema.MaxKeys)
        {
            CaptureMessage = "Already 3 keys: press one to remove it first";
            return;
        }
        else
        {
            keys.Add(bind);
        }

        Capturing = null;
        SetKey(item, keys.Count == 0 ? "none" : string.Join(", ", keys));
    }

    private string Default(string key) =>
        schema is not null && schema.ByKey.TryGetValue(key, out SettingOption? o) ? o.Default : "";

    /// <summary>
    /// Shows the chosen scheme's single player rows, and colours a key used by another row of the
    /// same tab for another button, as F3's <c>keys_table</c> does: rows of one section clash, and
    /// photo mode's two "anywhere" keys clash with every row.
    /// </summary>
    private void UpdateKeys()
    {
        string scheme = ValueOf("story_scheme");
        foreach (MenuTab tab in Tabs)
        {
            foreach (SettingItem item in tab.Items)
            {
                item.IsShown = item.Scheme is null || item.Scheme.Equals(scheme, StringComparison.OrdinalIgnoreCase);
            }

            var keys = tab.Items.OfType<KeyItem>().ToList();

            // Any of a row's keys (to-do 38) on another row clashes, and so do a combo's side
            // effects (CombosNotes; F3's combo_notes).
            foreach (KeyItem k in keys)
            {
                List<string> mine = SettingsSchema.SplitKeys(k.Value);
                var notes = new List<string>();
                if (k.IsShown && mine.Count > 0)
                {
                    foreach (KeyItem j in keys.Where(j => j != k && j.IsShown
                                                          && (j.Section == k.Section || j.ClashesAll || k.ClashesAll)
                                                          && j.Button != k.Button))
                    {
                        List<string> other = SettingsSchema.SplitKeys(j.Value);
                        if (mine.Any(a => other.Any(b => SettingsSchema.SameBind(a, b))))
                        {
                            notes.Add($"Also used for: {j.Label}");
                        }

                        ComboNotes(notes, mine, other, j.Label);
                    }
                }

                k.Notes = notes.Count == 0 ? null : string.Join("\n", notes.Distinct());
            }
        }
    }

    /// <summary>
    /// What a combo does to another row's keys (to-do 38), seen from either row: the hold key's
    /// own action happens too, and while the hold key is down the combo takes its second key from
    /// that key's own action.
    /// </summary>
    private void ComboNotes(List<string> notes, List<string> mine, List<string> other, string otherLabel)
    {
        foreach (string a in mine)
        {
            var (ah, ai, _) = SettingsSchema.BindParts(a);
            foreach (string b in other)
            {
                var (bh, bi, _) = SettingsSchema.BindParts(b);
                if (ah is not null && bh is null && bi == ah)
                {
                    notes.Add($"{Name(ah)} is also {otherLabel}: that happens too");
                }

                if (ah is not null && bh is null && bi == ai)
                {
                    notes.Add($"While {Name(ah)} is held, {Name(ai)} is this, not {otherLabel}");
                }

                if (ah is null && bh is not null && bh == ai)
                {
                    notes.Add($"{Name(ai)} also starts {otherLabel} ({BindLabel(b)}): this happens too");
                }

                if (ah is null && bh is not null && bi == ai)
                {
                    notes.Add($"While {Name(bh)} is held, {Name(ai)} is {otherLabel}, not this");
                }
            }
        }
    }
}
