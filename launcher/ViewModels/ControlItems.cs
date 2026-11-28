using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>Grey explanatory text, as F3 shows it.</summary>
public sealed class NoteItem(MenuEntry entry) : SettingItem(entry)
{
    public override void Show(string value)
    {
    }
}

/// <summary>A fixed key of menus, cutscenes or the pause screen (Menus tab): the action and all its keys.</summary>
public sealed class FixedItem(MenuEntry entry) : SettingItem(entry)
{
    public string Keys { get; } = entry.FixedKeys ?? "";

    public override void Show(string value)
    {
    }
}

public sealed partial class RadioOption(RadioItem owner, string value, string name) : ObservableObject
{
    public string Value { get; } = value;

    /// <summary>The choice's name in the language shown (the game's own words).</summary>
    public string Name => Lang.Menu(name);

    /// <summary>After a language switch: the name said again.</summary>
    public void SayAgain() => OnPropertyChanged(nameof(Name));

    public string Group => owner.Key!;

    [ObservableProperty]
    public partial bool IsChecked { get; set; }

    partial void OnIsCheckedChanged(bool value)
    {
        if (value)
        {
            owner.Picked(this);
        }
    }
}

/// <summary>A choice shown as radio buttons (the single player scheme).</summary>
public sealed class RadioItem : SettingItem
{
    private readonly ISettingsOwner owner;
    private bool showing;

    public RadioItem(ISettingsOwner owner, MenuEntry entry)
        : base(entry)
    {
        this.owner = owner;
        IReadOnlyList<string> values = entry.Values ?? [];
        for (int i = 0; i < values.Count; i++)
        {
            Options.Add(new RadioOption(this, values[i], entry.Names is { } n && i < n.Count ? n[i] : values[i]));
        }
    }

    public ObservableCollection<RadioOption> Options { get; } = [];

    public override void SayAgain()
    {
        base.SayAgain();
        foreach (RadioOption o in Options)
        {
            o.SayAgain();
        }
    }

    public override void Show(string value)
    {
        showing = true;
        foreach (RadioOption o in Options)
        {
            o.IsChecked = o.Value.Equals(value, StringComparison.OrdinalIgnoreCase);
        }

        showing = false;
    }

    internal void Picked(RadioOption option)
    {
        if (showing)
        {
            return;
        }

        foreach (RadioOption o in Options.Where(o => o != option))
        {
            o.IsChecked = false;
        }

        owner.Changed(Key!, option.Value);
    }
}

/// <summary>
/// An action and its key, as a row of F3's key tables: click the key to change it (the window
/// asks for the key), the reset icon puts back the default. Orange when another row of the tab
/// uses the same key for another button.
/// </summary>
public sealed partial class KeyItem(ControlsViewModel owner, MenuEntry entry, string defaultKey) : SettingItem(entry)
{
    private readonly string defaultKey = defaultKey;

    private readonly string? bracketButton =
        entry.ButtonShown && entry.Key?.StartsWith("story_", StringComparison.Ordinal) != true
        && entry.Button is { Length: > 0 } b ? b : null;

    /// <summary>The controller button in brackets ("(Back + D-pad Up)", said part by part); single player's rows have the Controller column instead.</summary>
    public string? ButtonText => bracketButton is null ? null : $"({Lang.Button(bracketButton)})";

    public string Button { get; } = entry.Button ?? "";

    public string? Section { get; } = entry.Section;

    /// <summary>Photo mode's two keys that work anywhere: they clash with every row of the tab.</summary>
    public bool ClashesAll { get; } = entry.ClashesAll;

    public string AutomationName => T("{0} key", Label);

    /// <summary>The keys' settings.ini value: up to three names, "space, mouse4" (to-do 38).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(KeyLabel), nameof(KeysNow), nameof(IsChanged), nameof(ResetTip))]
    public partial string Value { get; set; } = defaultKey;

    public string KeyLabel => owner.KeyLabel(Value);

    /// <summary>"Press a key"'s line with the keys it has now (key names stay English, as in F3).</summary>
    public string KeysNow => T("Keys now: {0}", Value == "none" ? T("none") : owner.KeyLabel(Value));

    public bool IsChanged => !Value.Equals(defaultKey, StringComparison.OrdinalIgnoreCase);

    public string ResetTip => T("Back to the default ({0})", owner.KeyLabel(defaultKey));

    /// <summary>What the row's keys do to other rows' (a shared key, a combo's side effects), or null.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsClash), nameof(ClashTip))]
    public partial string? Notes { get; set; }

    public bool IsClash => Notes is not null;

    /// <summary>Why it is orange, a line each: "Also used for: Jump", a combo's side effects (to-do 38).</summary>
    public string? ClashTip => Notes;

    public override void Show(string value) => Value = value;

    [RelayCommand]
    private void Capture() => owner.StartCapture(this);

    [RelayCommand]
    private void ResetKey() => owner.SetKey(this, defaultKey);

    // ---- the Controller column (to-do 39; single player's rows) --------------------------------

    /// <summary>Single player's rows have a Controller column: the action's button, or the controller's own.</summary>
    public bool ShowPad { get; } = entry.Key?.StartsWith("story_", StringComparison.Ordinal) == true;

    /// <summary>The controller bind's action ("jump"); null for a row the controller has fixed (the sticks, Back, the D-pad).</summary>
    public string? PadAction { get; } = entry.Pad;

    public bool HasPad => PadAction is not null;

    public bool ShowPadFixed => ShowPad && PadAction is null;

    private readonly string padFixed = entry.Button ?? "";

    /// <summary>The controller's own button for a fixed row ("Left stick up"), in the language shown.</summary>
    public string PadFixed => Lang.Button(padFixed);

    public string PadName => T("{0} controller button", Label);

    /// <summary>The bind's settings.ini value for the scheme shown ("a", "rt", "none").</summary>
    public string PadValue { get; private set; } = "none";

    public string PadLabel => owner.PadButtonLabel(PadValue);

    public bool PadChanged => HasPad && !PadValue.Equals(owner.PadDefault(this), StringComparison.OrdinalIgnoreCase);

    public string PadResetTip => T("Back to the default ({0})", owner.PadButtonLabel(owner.PadDefault(this)));

    /// <summary>Shows the bind's value (also after the scheme changed: the default may be another).</summary>
    internal void RefreshPad(string value)
    {
        PadValue = value;
        OnPropertyChanged(nameof(PadValue));
        OnPropertyChanged(nameof(PadLabel));
        OnPropertyChanged(nameof(PadChanged));
        OnPropertyChanged(nameof(PadResetTip));
    }

    [RelayCommand]
    private void CapturePad() => owner.StartPadCapture(this);

    [RelayCommand]
    private void ResetPad() => owner.SetPadBind(this, owner.PadDefault(this));
}

/// <summary>The column names above single player's key tables: Action, Keys, Controller (F3's header row).</summary>
public sealed class KeyHeaderItem(MenuEntry entry) : SettingItem(entry)
{
    public override void Show(string value)
    {
    }
}

/// <summary>
/// A controller button in the "press a button" pop-up's list; one another action has at that moment says whose.
/// Made when the pop-up opens (the language can't change while it is open).
/// </summary>
public sealed class PadOption(string name, string label, string? takenBy)
{
    public string Name { get; } = name;

    public string Label { get; } = label;

    public string Display { get; } = takenBy is null ? label : T("{0}  ({1}'s)", label, takenBy);
}

/// <summary>"Reset the controller buttons": asks first, then puts the scheme shown's binds back (to-do 39).</summary>
public sealed partial class ResetPadItem(ControlsViewModel owner, MenuEntry entry) : SettingItem(entry)
{
    public IReadOnlyList<string> Keys { get; } = entry.Keys ?? [];

    public string Question => owner.PadResetQuestion;

    [ObservableProperty]
    public partial bool IsAsking { get; set; }

    /// <summary>The last controller button change's swap, or why it was refused (shown above the button).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Message))]
    public partial Said? MessageSaid { get; set; }

    public string? Message => MessageSaid?.ToString();

    public override void Show(string value)
    {
    }

    [RelayCommand]
    private void Ask()
    {
        OnPropertyChanged(nameof(Question));
        IsAsking = true;
    }

    [RelayCommand]
    private void Cancel() => IsAsking = false;

    [RelayCommand]
    private void Confirm()
    {
        IsAsking = false;
        owner.ResetPadBinds(Keys);
    }
}

/// <summary>"Reset all ... controls": asks first, then puts its keys and mouse options back.</summary>
public sealed partial class ResetItem(ControlsViewModel owner, MenuEntry entry) : SettingItem(entry)
{
    private readonly string? question = entry.Question;

    public string Question => question is not null ? Lang.Menu(question) : T("Put everything back to the defaults?");

    public IReadOnlyList<string> Keys { get; } = entry.Keys ?? [];

    [ObservableProperty]
    public partial bool IsAsking { get; set; }

    public override void Show(string value)
    {
    }

    [RelayCommand]
    private void Ask() => IsAsking = true;

    [RelayCommand]
    private void Cancel() => IsAsking = false;

    [RelayCommand]
    private void Confirm()
    {
        IsAsking = false;
        owner.ResetKeys(Keys);
    }
}

/// <summary>
/// One player of the Slots tab: which controller it is, whether the keyboard and mouse are, and
/// its Options (F3's per-slot window: rumble, sticks, gyro; user 2026-11-15).
/// </summary>
public sealed partial class SlotRow(SlotsItem owner, int index) : ObservableObject
{
    private bool showing;

    public int Index { get; } = index;

    public string Name => T("Slot {0}", Index + 1);

    /// <summary>The entries of this slot's Options window, in F3's order.</summary>
    public ObservableCollection<SettingItem> Items { get; } = [];

    public string OptionsTitle => T("Slot {0} controller", Index + 1);

    public string OptionsName => T("Options for slot {0}", Index + 1);

    /// <summary>After a language switch: the row's words and its Options window's rows said again.</summary>
    public void SayAgain()
    {
        OnPropertyChanged(string.Empty);
        foreach (SettingItem item in Items)
        {
            item.SayAgain();
        }
    }

    /// <summary>The controller picked for the slot, as the list names it.</summary>
    public string ControllerText => Selected?.Name ?? "";

    [RelayCommand]
    private void OpenOptions() => owner.OpenOptions(this);

    public ObservableCollection<ChoiceOption> Options { get; } = [];

    [ObservableProperty]
    public partial ChoiceOption? Selected { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(KeyboardTip))]
    public partial bool KeyboardOn { get; set; }

    public string KeyboardName => T("Keyboard and mouse for slot {0}", Index + 1);

    public string KeyboardTip => KeyboardOn ? T("The keyboard and mouse are this player") : T("Make the keyboard and mouse this player");

    internal void Show(IEnumerable<ChoiceOption> options, string value, bool keyboard)
    {
        showing = true;
        Options.Clear();
        foreach (ChoiceOption o in options)
        {
            Options.Add(o);
        }

        Selected = Options.FirstOrDefault(o => o.Value.Equals(value, StringComparison.OrdinalIgnoreCase));
        KeyboardOn = keyboard;
        showing = false;
    }

    partial void OnSelectedChanged(ChoiceOption? value)
    {
        OnPropertyChanged(nameof(ControllerText));
        if (!showing && value is not null)
        {
            owner.Picked(this, value.Value);
        }
    }

    partial void OnKeyboardOnChanged(bool value)
    {
        if (!showing)
        {
            owner.KeyboardPicked(this, value);
        }
    }
}

/// <summary>
/// The Slots tab's table (F3's <c>ports_tab</c>): a controller per player, picked from the ones
/// <c>--list-pads</c> found; a controller is one slot's at a time, so picking one another slot
/// has puts that slot back to No input. The keyboard boxes are one choice: ticking one moves the
/// keyboard there, unticking it turns the keyboard off.
/// </summary>
public sealed class SlotsItem : SettingItem
{
    private readonly ControlsViewModel owner;
    private readonly string key;
    private readonly string keyboardKey;
    private readonly string keyboardOnKey;
    private readonly IReadOnlyList<string> values;

    public SlotsItem(ControlsViewModel owner, MenuEntry entry)
        : base(entry)
    {
        this.owner = owner;
        key = entry.Key ?? "controller_slot";
        keyboardKey = entry.KeyboardKey ?? "keyboard_slot";
        keyboardOnKey = entry.KeyboardOnKey ?? "keyboard_mouse";
        values = entry.Values ?? [];
        for (int i = 0; i < Math.Max(entry.Count, 1); i++)
        {
            Rows.Add(new SlotRow(this, i));
        }
    }

    public ObservableCollection<SlotRow> Rows { get; } = [];

    /// <summary>Every settings.ini key the table writes.</summary>
    public IEnumerable<string> Keys => Rows.Select(r => SlotKey(r.Index)).Append(keyboardKey).Append(keyboardOnKey);

    public override void Show(string value)
    {
    }

    /// <summary>After a language switch: every slot row (its controller list is named again by the tab's next <see cref="Update"/>).</summary>
    public override void SayAgain()
    {
        base.SayAgain();
        foreach (SlotRow row in Rows)
        {
            row.SayAgain();
        }
    }

    /// <summary>Shows the slots from the current values, the controllers named as --list-pads names them.</summary>
    internal void Update(Func<string, string> valueOf, IReadOnlyList<Pad> pads)
    {
        bool keyboard = valueOf(keyboardOnKey) != "0";
        string keyboardSlot = valueOf(keyboardKey);
        foreach (SlotRow row in Rows)
        {
            string value = valueOf(SlotKey(row.Index));
            // Only connected controllers, and the one set if it is gone, so the list says what the slot is set to.
            var options = values
                .Where(v => v is "none" or "any" || v.Equals(value, StringComparison.OrdinalIgnoreCase)
                            || pads.Any(p => p.Value == v && p.Connected))
                .Select(v => new ChoiceOption(v, Said.As(SourceName(v, pads))));
            row.Show(options, value, keyboard && keyboardSlot == (row.Index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
    }

    internal void Picked(SlotRow row, string value)
    {
        if (value != "none")
        {
            foreach (SlotRow other in Rows.Where(r => r != row && owner.ValueOf(SlotKey(r.Index)).Equals(value, StringComparison.OrdinalIgnoreCase)))
            {
                owner.Changed(SlotKey(other.Index), "none");
            }
        }

        owner.Changed(SlotKey(row.Index), value);
    }

    internal void OpenOptions(SlotRow row) => owner.OptionsRow = row;

    internal void KeyboardPicked(SlotRow row, bool on)
    {
        if (on)
        {
            owner.Changed(keyboardKey, (row.Index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture));
            owner.Changed(keyboardOnKey, "1");
        }
        else
        {
            owner.Changed(keyboardOnKey, "0");
        }
    }

    private string SlotKey(int index) => key + (index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture);

    // A connected controller's own name (Windows' / SDL's) stays as it is.
    private static string SourceName(string value, IReadOnlyList<Pad> pads) => value switch
    {
        "none" => T("No input"),
        "any" => T("Any controller"),
        _ => pads.FirstOrDefault(p => p.Value == value)?.Name
             ?? (value.StartsWith("ps", StringComparison.Ordinal) ? T("PlayStation / Switch controller {0}", value[2..]) : T("Controller {0}", value)),
    };
}

/// <summary>F3 Slots' Chords button (to-do 39): opens the Controls tab's Chords pop-up.</summary>
public sealed partial class ChordsItem(ControlsViewModel owner, MenuEntry entry) : SettingItem(entry)
{
    public override void Show(string value)
    {
    }

    [RelayCommand]
    private void Open() => owner.OpenChords();
}

/// <summary>A group of the Chords pop-up (Universal, Photo Mode, Cheats / Debug) with its rows; its words in the language shown.</summary>
public sealed partial class ChordGroupView(string name, string tip) : ObservableObject
{
    public string Name => Lang.Menu(name);

    public string Tip => Lang.Menu(tip);

    public ObservableCollection<ChordRow> Rows { get; } = [];

    /// <summary>After a language switch: the words said again.</summary>
    public void SayAgain() => OnPropertyChanged(string.Empty);
}

/// <summary>A chord in a row's list; one another function has and may not share is greyed, with its name.</summary>
public sealed partial class ChordOption(string name, Said label) : ObservableObject
{
    public string Name { get; } = name;

    public string Label => label.ToString();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsFree), nameof(Display))]
    public partial string? TakenBy { get; set; }

    public bool IsFree => TakenBy is null;

    public string Display => TakenBy is null ? Label : $"{Label}  ({TakenBy})";

    /// <summary>After a language switch: the label said again.</summary>
    public void SayAgain()
    {
        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(Display));
    }
}

/// <summary>
/// One function of the Chords pop-up (F3's <c>chord_row</c>): its chord from a list, Press (hold
/// the chord on a controller), and back to the default.
/// </summary>
public sealed partial class ChordRow : ObservableObject
{
    private readonly ControlsViewModel owner;
    private bool showing;

    public ChordRow(ControlsViewModel owner, ChordFunction function, IReadOnlyList<ChordChoice> chords, string defaultValue)
    {
        this.owner = owner;
        Function = function;
        Default = defaultValue;
        if (!function.Required)
        {
            Options.Add(new ChordOption("none", S("None")));
        }

        foreach (ChordChoice c in chords)
        {
            Options.Add(new ChordOption(c.Name, Said.Button(c.Label)));
        }
    }

    /// <summary>After a language switch: the row's words and its list's said again.</summary>
    public void SayAgain()
    {
        OnPropertyChanged(string.Empty);
        foreach (ChordOption o in Options)
        {
            o.SayAgain();
        }
    }

    public ChordFunction Function { get; }

    public string Key => Function.Key;

    public string Label => Lang.Menu(Function.Label);

    public string Tip => Lang.Menu(Function.Tip);

    public bool Required => Function.Required;

    public string Default { get; }

    public string Name => T("{0} chord", Label);

    public string PressName => T("Press the chord for {0}", Label);

    public string ResetName => T("{0}: back to the default", Label);

    public ObservableCollection<ChordOption> Options { get; } = [];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsDefault), nameof(ChordText), nameof(ChordNowText))]
    public partial ChordOption? Selected { get; set; }

    public bool IsDefault => Selected?.Name == Default;

    /// <summary>What it has now, as the Press question shows it.</summary>
    public string ChordText => Selected is { Name: not "none" } s ? s.Label : T("none");

    /// <summary>The Press question's line: "Chord now: Back + Start".</summary>
    public string ChordNowText => T("Chord now: {0}", ChordText);

    public string ResetTip => T("Back to the default ({0})", owner.ChordLabel(Default));

    partial void OnSelectedChanged(ChordOption? value)
    {
        if (!showing && value is not null)
        {
            owner.SetChord(this, value.Name);
        }
    }

    /// <summary>Shows a settings.ini value (not one the list has: the default).</summary>
    internal void Show(string value)
    {
        showing = true;
        Selected = Options.FirstOrDefault(o => o.Name.Equals(value, StringComparison.OrdinalIgnoreCase))
                   ?? Options.FirstOrDefault(o => o.Name == Default);
        showing = false;
    }

    [RelayCommand]
    private void Press() => owner.StartChordCapture(this);

    [RelayCommand]
    private void Reset() => owner.SetChord(this, Default);
}

