using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>One tab of the F3 menu (Single player, Multiplayer, Photo mode, Menus, Slots).</summary>
public sealed class ControlsTab(string name)
{
    public string Name { get; } = name;

    public ObservableCollection<SettingItem> Items { get; } = [];
}

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

    public string Name { get; } = name;

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

    public string? ButtonText { get; } = entry.ButtonShown && entry.Button is { Length: > 0 } b ? $"({b})" : null;

    public string Button { get; } = entry.Button ?? "";

    public string? Section { get; } = entry.Section;

    /// <summary>The single player scheme the row belongs to; null for both.</summary>
    public string? Scheme { get; } = entry.Scheme;

    /// <summary>Photo mode's two keys that work anywhere: they clash with every row of the tab.</summary>
    public bool ClashesAll { get; } = entry.ClashesAll;

    public string AutomationName => $"{Label} key";

    /// <summary>The key's settings.ini name.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(KeyLabel), nameof(IsChanged), nameof(ResetTip))]
    public partial string Value { get; set; } = defaultKey;

    public string KeyLabel => owner.KeyLabel(Value);

    public bool IsChanged => !Value.Equals(defaultKey, StringComparison.OrdinalIgnoreCase);

    public string ResetTip => $"Back to the default ({owner.KeyLabel(defaultKey)})";

    /// <summary>Shown under the chosen scheme (or both).</summary>
    [ObservableProperty]
    public partial bool IsShown { get; set; } = true;

    /// <summary>Another row's label using the same key for another button, if any.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsClash), nameof(ClashTip))]
    public partial string? ClashWith { get; set; }

    public bool IsClash => ClashWith is not null;

    public string? ClashTip => ClashWith is null ? null : $"Also used for: {ClashWith}";

    public override void Show(string value) => Value = value;

    [RelayCommand]
    private void Capture() => owner.StartCapture(this);

    [RelayCommand]
    private void ResetKey() => owner.SetKey(this, defaultKey);
}

/// <summary>"Reset all ... controls": asks first, then puts its keys and mouse options back.</summary>
public sealed partial class ResetItem(ControlsViewModel owner, MenuEntry entry) : SettingItem(entry)
{
    public string Question { get; } = entry.Question ?? "Put everything back to the defaults?";

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

/// <summary>One player of the Slots tab: which controller it is and whether the keyboard and mouse are.</summary>
public sealed partial class SlotRow(SlotsItem owner, int index) : ObservableObject
{
    private bool showing;

    public int Index { get; } = index;

    public string Name => $"Slot {Index + 1}";

    public ObservableCollection<ChoiceOption> Options { get; } = [];

    [ObservableProperty]
    public partial ChoiceOption? Selected { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(KeyboardTip))]
    public partial bool KeyboardOn { get; set; }

    public string KeyboardName => $"Keyboard and mouse for slot {Index + 1}";

    public string KeyboardTip => KeyboardOn ? "The keyboard and mouse are this player" : "Make the keyboard and mouse this player";

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
                .Select(v => new ChoiceOption(v, SourceName(v, pads)));
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

    private static string SourceName(string value, IReadOnlyList<Pad> pads) => value switch
    {
        "none" => "No input",
        "any" => "Any controller",
        _ => pads.FirstOrDefault(p => p.Value == value)?.Name
             ?? (value.StartsWith("ps", StringComparison.Ordinal) ? $"PlayStation controller {value[2..]}" : $"Controller {value}"),
    };
}
