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
    private ChordsInfo? chords;
    private readonly List<ChordRow> chordRows = [];
    private readonly HashSet<string> chordHeld = [];   // "Press the chord": buttons held now, pressed since it asked
    private readonly HashSet<string> chordSeen = [];   // ... and every one pressed since all were let go
    private bool chordGot;                              // a chord was made since all were let go

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

    /// <summary>The slot whose Options pop-up shows (F3's per-slot window); null = none.</summary>
    [ObservableProperty]
    public partial SlotRow? OptionsRow { get; set; }

    [RelayCommand]
    private void CloseOptions() => OptionsRow = null;

    // ---- F3 Slots > Chords (to-do 39; the user's plan 2026-11-17) ------------------------------

    /// <summary>The Chords pop-up's groups (the functions that show; F3's <c>chords_window</c>).</summary>
    public ObservableCollection<ChordGroupView> ChordGroups { get; } = [];

    [ObservableProperty]
    public partial bool ChordsOpen { get; set; }

    /// <summary>Why the last chord picked wasn't taken.</summary>
    [ObservableProperty]
    public partial string? ChordsMessage { get; set; }

    /// <summary>"Put every chord back to its default?" showing.</summary>
    [ObservableProperty]
    public partial bool AskResetChords { get; set; }

    /// <summary>The row whose chord is being pressed ("Press"), or null.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsChordCapturing))]
    public partial ChordRow? ChordCapturing { get; set; }

    public bool IsChordCapturing => ChordCapturing is not null;

    /// <summary>Why the last buttons pressed for it weren't taken.</summary>
    [ObservableProperty]
    public partial string? ChordCaptureMessage { get; set; }

    internal void OpenChords()
    {
        ChordsMessage = null;
        AskResetChords = false;
        ChordsOpen = true;
    }

    [RelayCommand]
    private void CloseChords()
    {
        ChordsOpen = false;
        AskResetChords = false;
        ChordsMessage = null;
    }

    [RelayCommand]
    private void ResetChords() => AskResetChords = true;

    [RelayCommand]
    private void ConfirmResetChords()
    {
        foreach (ChordRow row in chordRows)
        {
            Changed(row.Key, row.Default);
        }

        AskResetChords = false;
        ChordsMessage = null;
    }

    [RelayCommand]
    private void CancelResetChords() => AskResetChords = false;

    /// <summary>A chord's label ("Back + Start"), "none" for none.</summary>
    internal string ChordLabel(string name) =>
        chords?.Chords.FirstOrDefault(c => c.Name.Equals(name, StringComparison.OrdinalIgnoreCase))?.Label ?? "none";

    /// <summary>Two functions may share a chord when neither is Universal (group 0) and their groups differ.</summary>
    private static bool MayShare(ChordFunction a, ChordFunction b) => a.Group != 0 && b.Group != 0 && a.Group != b.Group;

    private ChordRow? TakenBy(ChordRow row, string name) =>
        name == "none" ? null
        : chordRows.FirstOrDefault(r => r != row && r.Selected?.Name == name && !MayShare(row.Function, r.Function));

    /// <summary>A row's chord picked (list, Press or reset): refused when another function has it and may not share it.</summary>
    internal void SetChord(ChordRow row, string name)
    {
        if (TakenBy(row, name) is { } other)
        {
            ChordsMessage = $"{ChordLabel(name)} is {other.Label}'s: change that one first";
            Dispatcher.UIThread.Post(UpdateChords);     // the list shows the old one again
            return;
        }

        if (name == "none" && row.Required)
        {
            Dispatcher.UIThread.Post(UpdateChords);
            return;
        }

        ChordsMessage = null;
        Changed(row.Key, name);
    }

    private ChordsItem BuildChords(MenuEntry entry)
    {
        chords = entry.Chords;
        ChordGroups.Clear();
        chordRows.Clear();
        if (chords is not null)
        {
            for (int g = 0; g < chords.Groups.Count; g++)
            {
                var view = new ChordGroupView(chords.Groups[g].Name, chords.Groups[g].Tip);
                foreach (ChordFunction fn in chords.Functions.Where(fn => fn.Group == g))
                {
                    var row = new ChordRow(this, fn, chords.Chords, Default(fn.Key));
                    view.Rows.Add(row);
                    chordRows.Add(row);
                }

                if (view.Rows.Count > 0)
                {
                    ChordGroups.Add(view);
                }
            }
        }

        return new ChordsItem(this, entry);
    }

    /// <summary>Every row's chord from the values, and which chords each row's list greys.</summary>
    private void UpdateChords()
    {
        foreach (ChordRow row in chordRows)
        {
            row.Show(ValueOf(row.Key));
        }

        foreach (ChordRow row in chordRows)
        {
            foreach (ChordOption option in row.Options)
            {
                option.TakenBy = TakenBy(row, option.Name)?.Label;
            }
        }
    }

    internal void StartChordCapture(ChordRow row)
    {
        ChordCaptureMessage = null;
        chordHeld.Clear();
        chordSeen.Clear();
        chordGot = false;
        ChordCapturing = row;
    }

    [RelayCommand]
    private void CancelChordCapture() => ChordCapturing = null;

    [RelayCommand]
    private void NoChord()
    {
        if (ChordCapturing is { Required: false } row)
        {
            SetChord(row, "none");
        }

        ChordCapturing = null;
    }

    private static readonly Dictionary<PadButton, (string Name, string Label)> ChordButtons = new()
    {
        [PadButton.A] = ("a", "A"), [PadButton.B] = ("b", "B"), [PadButton.X] = ("x", "X"), [PadButton.Y] = ("y", "Y"),
        [PadButton.LB] = ("lb", "LB"), [PadButton.RB] = ("rb", "RB"), [PadButton.Start] = ("start", "Start"),
        [PadButton.Back] = ("back", "Back"), [PadButton.Up] = ("dpad_up", "D-pad Up"),
        [PadButton.Down] = ("dpad_down", "D-pad Down"), [PadButton.Left] = ("dpad_left", "D-pad Left"),
        [PadButton.Right] = ("dpad_right", "D-pad Right"), [PadButton.LeftStick] = ("ls", "LS"),
        [PadButton.RightStick] = ("rs", "RS"),
    };

    /// <summary>
    /// "Press the chord": a button pressed (PadNavigator hands every press here meanwhile). Two
    /// held together that make a chord answer it; only buttons pressed since it asked count (the
    /// A that pressed Press doesn't).
    /// </summary>
    public void TakeChordPress(PadButton button)
    {
        if (ChordCapturing is not { } row || chords is null || !ChordButtons.TryGetValue(button, out var b))
        {
            return;
        }

        chordHeld.Add(b.Name);
        chordSeen.Add(b.Name);
        if (chordGot || chordHeld.Count != 2)
        {
            return;
        }

        ChordChoice? chord = chords.Chords.FirstOrDefault(c => c.Name.Split('+').ToHashSet().SetEquals(chordHeld));
        if (chord is null)
        {
            return;
        }

        chordGot = true;
        if (chord.Name == row.Selected?.Name)
        {
            ChordCapturing = null;
        }
        else if (TakenBy(row, chord.Name) is { } other)
        {
            ChordCaptureMessage = $"{chord.Label} is {other.Label}'s: pick another";
        }
        else
        {
            SetChord(row, chord.Name);
            ChordCapturing = null;
        }
    }

    /// <summary>"Press the chord": a button let go. All let go without a chord: B alone cancels, else why not.</summary>
    public void TakeChordRelease(PadButton button)
    {
        if (ChordCapturing is null || !ChordButtons.TryGetValue(button, out var b) || !chordHeld.Remove(b.Name)
            || chordHeld.Count > 0)
        {
            return;
        }

        if (!chordGot)
        {
            if (chordSeen.SetEquals(["b"]))
            {
                ChordCapturing = null;
            }
            else
            {
                string names = string.Join(" + ", ChordButtons.Values.Where(v => chordSeen.Contains(v.Name)).Select(v => v.Label));
                ChordCaptureMessage = chordSeen.Count > 1
                    ? $"{names} is not one of the chords"
                    : $"{names} alone: hold it and press a second button";
            }
        }

        chordSeen.Clear();
        chordGot = false;
    }

    // ---- Controller binds (to-do 39; the user's design 2026-11-17): single player's ------------
    // ---- Controller column, F3's set_pad_bind / ask_pad_bind ------------------------------------

    private PadBindsInfo? padBinds;
    private readonly List<KeyItem> padRows = [];
    private bool padShowing;

    private bool ModernScheme => ValueOf("story_scheme").Equals("modern", StringComparison.OrdinalIgnoreCase);

    private PadAction? PadActionOf(string name) => padBinds?.Actions.FirstOrDefault(a => a.Name == name);

    /// <summary>An action's settings.ini key in a scheme ("pad_classic_jump"), null when the scheme has no such action.</summary>
    private string? PadKey(string action, bool modern) =>
        PadActionOf(action) is { } a && (modern ? a.Modern : a.Classic) is not null
            ? $"pad_{(modern ? "modern" : "classic")}_{action}"
            : null;

    private string PadActionLabel(string action, bool modern) =>
        PadActionOf(action) is { } a ? (modern ? a.Modern : a.Classic) ?? action : action;

    internal string PadDefault(KeyItem k) =>
        k.PadAction is { } a && PadKey(a, ModernScheme) is { } key ? Default(key) : "none";

    /// <summary>A button's label ("Right trigger"), "-" for none.</summary>
    internal string PadButtonLabel(string name) =>
        padBinds?.Buttons.FirstOrDefault(b => b.Name.Equals(name, StringComparison.OrdinalIgnoreCase))?.Label ?? "-";

    internal string PadResetQuestion =>
        $"Put every {(ModernScheme ? "Modern" : "Classic")} controller button back to its default?";

    /// <summary>
    /// The actions that would clash with action on button (F3's <c>pad_binds_clashes</c>): they act at
    /// the same moment. Crouch and Attack never do, nor a section's action with the main list's or
    /// one of another place.
    /// </summary>
    private List<string> PadClashes(string action, bool modern, string button)
    {
        var list = new List<string>();
        if (button == "none" || PadActionOf(action) is not { } me || PadKey(action, modern) is null || padBinds is null)
        {
            return list;
        }

        foreach (PadAction a in padBinds.Actions)
        {
            if (a.Name == action || PadKey(a.Name, modern) is not { } key
                || !ValueOf(key).Equals(button, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if ((me.Place == 0) != (a.Place == 0) || (me.Place != 0 && me.Place != a.Place) || me.Shares == a.Name)
            {
                continue;
            }

            list.Add(a.Name);
        }

        return list;
    }

    /// <summary>The last controller button change's swap, or why it was refused.</summary>
    [ObservableProperty]
    public partial string? PadMessage { get; set; }

    private ResetPadItem? resetPad;

    partial void OnPadMessageChanged(string? value)
    {
        if (resetPad is not null)
        {
            resetPad.Message = value;
        }
    }

    /// <summary>The row whose controller button is being asked for ("press a button"), or null.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPadCapturing), nameof(PadCaptureTitle), nameof(PadNowText))]
    public partial KeyItem? PadCapturing { get; set; }

    public bool IsPadCapturing => PadCapturing is not null;

    public string PadCaptureTitle =>
        PadCapturing is { } k ? $"{k.Label} ({(ModernScheme ? "Modern" : "Classic")})" : "";

    public string PadNowText =>
        PadCapturing is { } k ? $"Button now: {(k.PadValue == "none" ? "none" : PadButtonLabel(k.PadValue))}" : "";

    public bool PadNoneAllowed => PadCapturing?.PadAction is { } a && PadActionOf(a) is { Required: false };

    /// <summary>Why the last button pressed for it wasn't taken.</summary>
    [ObservableProperty]
    public partial string? PadCaptureMessage { get; set; }

    /// <summary>The pop-up's list (for the mouse): every button, with whose it is at that moment.</summary>
    public ObservableCollection<PadOption> PadOptions { get; } = [];

    [ObservableProperty]
    public partial PadOption? PadSelected { get; set; }

    partial void OnPadSelectedChanged(PadOption? value)
    {
        if (!padShowing && value is not null && PadCapturing is { } k)
        {
            if (SetPadBind(k, value.Name))
            {
                PadCapturing = null;
            }
            else
            {
                PadCaptureMessage = PadMessage;
                // Refused: the list shows the button it has again.
                Dispatcher.UIThread.Post(() =>
                {
                    padShowing = true;
                    PadSelected = PadOptions.FirstOrDefault(o => o.Name.Equals(k.PadValue, StringComparison.OrdinalIgnoreCase));
                    padShowing = false;
                });
            }
        }
    }

    internal void StartPadCapture(KeyItem k)
    {
        PadCaptureMessage = null;
        PadMessage = null;
        PadCapturing = k;
        OnPropertyChanged(nameof(PadNoneAllowed));
        padShowing = true;
        PadOptions.Clear();
        bool modern = ModernScheme;
        foreach (PadChoice b in padBinds?.Buttons ?? [])
        {
            List<string> c = PadClashes(k.PadAction!, modern, b.Name);
            PadOptions.Add(new PadOption(b.Name, b.Label, c.Count > 0 ? PadActionLabel(c[0], modern) : null));
        }

        PadSelected = PadOptions.FirstOrDefault(o => o.Name.Equals(k.PadValue, StringComparison.OrdinalIgnoreCase));
        padShowing = false;
    }

    [RelayCommand]
    private void CancelPadCapture() => PadCapturing = null;

    [RelayCommand]
    private void NoPadButton()
    {
        if (PadCapturing is { } k)
        {
            if (SetPadBind(k, "none"))
            {
                PadCapturing = null;
            }
            else
            {
                PadCaptureMessage = PadMessage;
            }
        }
    }

    private static readonly Dictionary<PadButton, string> PadNames = new()
    {
        [PadButton.A] = "a", [PadButton.B] = "b", [PadButton.X] = "x", [PadButton.Y] = "y",
        [PadButton.LB] = "lb", [PadButton.RB] = "rb", [PadButton.LT] = "lt", [PadButton.RT] = "rt",
        [PadButton.LeftStick] = "ls", [PadButton.RightStick] = "rs", [PadButton.Start] = "start",
    };

    /// <summary>
    /// "Press a button": a controller button pressed (PadNavigator hands every press here
    /// meanwhile). Back cancels; the D-pad can't be bound; any other is the answer.
    /// </summary>
    public void TakePadPress(PadButton button)
    {
        if (PadCapturing is not { } k)
        {
            return;
        }

        if (button == PadButton.Back)
        {
            PadCapturing = null;
        }
        else if (button is PadButton.Up or PadButton.Down or PadButton.Left or PadButton.Right)
        {
            PadCaptureMessage = "The D-pad can't be bound: pick another button (Back cancels)";
        }
        else if (PadNames.TryGetValue(button, out string? name))
        {
            if (SetPadBind(k, name))
            {
                PadCapturing = null;
            }
            else
            {
                PadCaptureMessage = PadMessage;
            }
        }
    }

    /// <summary>
    /// One action's button for the scheme shown (F3's <c>set_pad_bind</c>): another action with it at
    /// the same moment swaps to this one's old button; one that can't take it (it clashes there too,
    /// or this one had none) is left with none; Pause is never left without one (refused). False
    /// when refused (PadMessage says why).
    /// </summary>
    internal bool SetPadBind(KeyItem k, string button)
    {
        bool modern = ModernScheme;
        string action = k.PadAction!;
        if (PadKey(action, modern) is not { } key)
        {
            return false;
        }

        string old = ValueOf(key);
        PadMessage = null;
        if (old.Equals(button, StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        if (button == "none" && PadActionOf(action)?.Required == true)
        {
            PadMessage = $"{PadActionLabel(action, modern)} needs a button";
            return false;
        }

        var to = new List<(string Action, string Button)>();
        foreach (string c in PadClashes(action, modern, button))
        {
            bool clean = old != "none" && PadClashes(c, modern, old).All(o => o == action);
            if (!clean && PadActionOf(c)?.Required == true)
            {
                string l = PadActionLabel(c, modern);
                PadMessage = $"{PadButtonLabel(button)} is {l}'s, and {l} needs a button: give {l} another first";
                return false;
            }

            to.Add((c, clean ? old : "none"));
        }

        var what = new List<string>();
        foreach ((string c, string v) in to)
        {
            Changed(PadKey(c, modern)!, v);
            what.Add(v == "none"
                ? $"{PadActionLabel(c, modern)} has no button now: pick one"
                : $"{PadActionLabel(c, modern)} takes {PadButtonLabel(v)}");
        }

        Changed(key, button);
        PadMessage = what.Count > 0 ? string.Join("; ", what) : null;
        return true;
    }

    /// <summary>"Reset the controller buttons": the scheme shown's binds back to their defaults.</summary>
    internal void ResetPadBinds(IEnumerable<string> keys)
    {
        string prefix = ModernScheme ? "pad_modern_" : "pad_classic_";
        foreach (string key in keys.Where(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)))
        {
            Changed(key, Default(key));
        }

        PadMessage = null;
    }

    /// <summary>Every row's controller button for the scheme shown.</summary>
    private void UpdatePads()
    {
        bool modern = ModernScheme;
        foreach (KeyItem k in padRows)
        {
            k.RefreshPad(PadKey(k.PadAction!, modern) is { } key ? ValueOf(key) : "none");
        }
    }

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
        OptionsRow = null;
        ChordsOpen = false;
        ChordCapturing = null;
        PadCapturing = null;
        padBinds = null;
        padRows.Clear();
        resetPad = null;
        MenuTab? tab = null;
        // A slot's Options entries (F3 puts them in that slot's window): kept for its row.
        var slotItems = new List<(int Slot, SettingItem Item)>();
        foreach (MenuEntry entry in schema.Controls)
        {
            if (entry.Type == "tab")
            {
                tab = new MenuTab(entry.Label);
                Tabs.Add(tab);
                continue;
            }

            if (entry.Type == "pad_binds")
            {
                padBinds = entry.PadBinds;
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
                "reset_pad" => resetPad = new ResetPadItem(this, entry),
                "slots" => slots = new SlotsItem(this, entry),
                "chords" => BuildChords(entry),
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
            if (entry.Slot > 0)
            {
                slotItems.Add((entry.Slot, item));
                continue;
            }

            ObservableCollection<SettingItem> list = tab?.Items ?? TopItems;
            // Single player's key tables get the column names above them (F3's header row).
            if (item is KeyItem { ShowPad: true } k)
            {
                if (list.Count == 0 || list[^1] is not (KeyItem or KeyHeaderItem))
                {
                    list.Add(new KeyHeaderItem(new MenuEntry("keys_header", "Action", null, null, null, null, null,
                                                             0, 0, null, 0, null, null)));
                }

                if (k.HasPad)
                {
                    padRows.Add(k);
                }
            }

            list.Add(item);
        }

        foreach ((int slot, SettingItem item) in slotItems)
        {
            if (slots is not null && slot <= slots.Rows.Count)
            {
                slots.Rows[slot - 1].Items.Add(item);
            }
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
        UpdateChords();
        PadMessage = null;
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

            if (chordRows.Any(r => r.Key.Equals(key, StringComparison.OrdinalIgnoreCase)))
            {
                UpdateChords();
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
        UpdatePads();
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
