using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>A tab whose rows write settings.ini (Settings, Controls).</summary>
public interface ISettingsOwner
{
    /// <summary>An item's new value, from the player: saved shortly.</summary>
    void Changed(string key, string value);
}

/// <summary>
/// One tab of the F2 menu (Display, Graphics, Sound, Fixes) or the F3 menu (Single player,
/// Multiplayer, Photo mode, Menus, Slots), shown inside the Settings or Controls tab.
/// </summary>
public sealed partial class MenuTab(string name) : ObservableObject
{
    /// <summary>The tab's name in the language shown (the game's own words; to-do 289 step 3).</summary>
    public string Name => Lang.Menu(name);

    /// <summary>After a language switch: the name said again.</summary>
    public void SayAgain() => OnPropertyChanged(nameof(Name));

    public ObservableCollection<SettingItem> Items { get; } = [];

    /// <summary>
    /// Where its list opens: at the top. The tabs share one scroll view, which otherwise opened a
    /// tab as far down as the last one was scrolled (a remembered place per tab came back cut short
    /// by the other tab's length).
    /// </summary>
    public Avalonia.Vector ScrollOffset { get; }
}

/// <summary>One row of the Settings or Controls tab.</summary>
public abstract partial class SettingItem(MenuEntry entry) : ObservableObject
{
    private readonly string label = entry.Label;
    private readonly string? tip = entry.Tip;
    private string? help;

    /// <summary>The row's name in the language shown: the game's own translation of what --dump-settings printed (to-do 289 step 3).</summary>
    public string Label => Lang.Menu(label);

    public string? Key { get; } = entry.Key;

    public string? Needs { get; } = entry.Needs;

    /// <summary>
    /// The menu's tooltip in the language shown, else the option's settings.ini comment, in the
    /// language shown too (to-do 289 step 4: the game marks those comments for translation; the
    /// file itself keeps them English).
    /// </summary>
    public string? Tip => tip is not null ? Lang.Menu(tip) : help is null ? null : Lang.Help(help);

    /// <summary>The settings.ini comment, the tooltip when the menu has none.</summary>
    public void SetHelp(string? text) => help ??= text;

    /// <summary>False when what it needs (the decensor patch) isn't there.</summary>
    [ObservableProperty]
    public partial bool IsAvailable { get; set; } = true;

    /// <summary>The single player scheme the row belongs to; null for both.</summary>
    public string? Scheme { get; } = entry.Scheme;

    /// <summary>The checkbox this row sits under (indented, shown only while it is on); null for none.</summary>
    public string? Parent { get; } = entry.Parent;

    /// <summary>When the parent is a choice: the value it must have for this row to show.</summary>
    public string? ParentValue { get; } = entry.ParentValue;

    /// <summary>How many parents up the row has (set by the tab as it builds its rows, before showing them).</summary>
    public int Depth { get; set; }

    /// <summary>A checkbox's margin: indented under its parent.</summary>
    public Avalonia.Thickness Indent => new(Depth * 24, 2, 0, 2);

    /// <summary>A slider's margin: indented under its parent.</summary>
    public Avalonia.Thickness SliderIndent => new(Depth * 24, 4, 0, 4);

    /// <summary>Shown under the chosen scheme (or both), and while its parent is on.</summary>
    [ObservableProperty]
    public partial bool IsShown { get; set; } = true;

    /// <summary>Shows a value from the file without saving it.</summary>
    public abstract void Show(string value);

    /// <summary>
    /// After a language switch (to-do 289): every word of the row said again. The tab calls it on
    /// each of its rows; a row with options or rows of its own passes it on.
    /// </summary>
    public virtual void SayAgain() => OnPropertyChanged(string.Empty);
}

public sealed class SectionItem(MenuEntry entry) : SettingItem(entry)
{
    public override void Show(string value)
    {
    }
}

/// <summary>
/// A heading with a checkbox in front that sets a group of options at once (F2 Extras' "Unload
/// Reloaded", user 2026-11-25): ticked while every option has its "on" value; ticking sets them
/// all, unticking puts each back to its "off" value.
/// </summary>
public sealed partial class GroupItem(MenuEntry entry, Action<GroupItem, bool> apply) : SettingItem(entry)
{
    private bool showing;

    /// <summary>Each option the group sets: its key, its value when ticked, its value when unticked.</summary>
    public IReadOnlyList<(string Key, string On, string Off)> Set { get; } =
        [.. (entry.GroupKeys ?? []).Select((k, i) => (k, entry.GroupOn?[i] ?? "1", entry.GroupOff?[i] ?? "0"))];

    [ObservableProperty]
    public partial bool IsOn { get; set; }

    public override void Show(string value)
    {
    }

    /// <summary>Ticked while every option has its "on" value ("1" = a checkbox that is on).</summary>
    public void Update(IReadOnlyDictionary<string, string> values)
    {
        bool all = Set.Count > 0 && Set.All(s => values.TryGetValue(s.Key, out string? v)
            && (s.On == "1" ? v != "0" : string.Equals(v, s.On, StringComparison.OrdinalIgnoreCase)));
        showing = true;
        IsOn = all;
        showing = false;
    }

    partial void OnIsOnChanged(bool value)
    {
        if (!showing)
        {
            apply(this, value);
        }
    }
}

/// <summary>A plain line between groups of options, with no heading (F2's Fixes tab).</summary>
public sealed class LineItem(MenuEntry entry) : SettingItem(entry)
{
    public override void Show(string value)
    {
    }
}

public sealed partial class CheckItem(ISettingsOwner owner, MenuEntry entry) : SettingItem(entry)
{
    private bool showing;

    [ObservableProperty]
    public partial bool IsOn { get; set; }

    public override void Show(string value)
    {
        showing = true;
        IsOn = value != "0";
        showing = false;
    }

    partial void OnIsOnChanged(bool value)
    {
        if (!showing)
        {
            owner.Changed(Key!, value ? "1" : "0");
        }
    }
}

/// <summary>One entry of a drop-down: its settings.ini value and its name in the language shown.</summary>
public sealed partial class ChoiceOption(string value, Said name) : ObservableObject
{
    public string Value { get; } = value;

    public string Name => name.ToString();

    /// <summary>After a language switch: the name said again.</summary>
    public void SayAgain() => OnPropertyChanged(nameof(Name));
}

public sealed partial class ChoiceItem : SettingItem
{
    private readonly ISettingsOwner owner;
    private bool showing;

    public ChoiceItem(ISettingsOwner owner, MenuEntry entry)
        : base(entry)
    {
        this.owner = owner;
        IReadOnlyList<string> values = entry.Values ?? [];
        IReadOnlyList<string> names = entry.Names ?? values;
        for (int i = 0; i < values.Count; i++)
        {
            Options.Add(new ChoiceOption(values[i], Said.Menu(i < names.Count ? names[i] : values[i])));
        }
    }

    public ObservableCollection<ChoiceOption> Options { get; } = [];

    public override void SayAgain()
    {
        base.SayAgain();
        foreach (ChoiceOption o in Options)
        {
            o.SayAgain();
        }
    }

    [ObservableProperty]
    public partial ChoiceOption? Selected { get; set; }

    public override void Show(string value)
    {
        // As in F2, a value only reachable by editing the file is listed too.
        ChoiceOption? match = Options.FirstOrDefault(o => o.Value.Equals(value, StringComparison.OrdinalIgnoreCase));
        if (match is null)
        {
            match = new ChoiceOption(value, Said.As(value));
            Options.Add(match);
        }

        showing = true;
        Selected = match;
        showing = false;
    }

    partial void OnSelectedChanged(ChoiceOption? value)
    {
        if (!showing && value is not null)
        {
            owner.Changed(Key!, value.Value);
        }
    }
}

public sealed partial class SliderItem(ISettingsOwner owner, MenuEntry entry) : SettingItem(entry)
{
    private readonly int decimals = entry.Decimals;
    private readonly string unit = entry.Unit ?? "";
    private readonly string neutral = entry.Neutral ?? "0";
    private bool showing;

    public double Minimum { get; } = entry.Min;

    public double Maximum { get; } = entry.Max;

    public double Step { get; } = Math.Pow(10, -entry.Decimals);

    private readonly string? resetTip = entry.ResetTip;

    public string? ResetTip => resetTip is null ? null : Lang.Menu(resetTip);

    /// <summary>A sound channel's mute key: a Mute box beside the name (to-do 13); null for none.</summary>
    public string? MuteKey { get; } = entry.Mute;

    public bool HasMute => MuteKey is not null;

    public string MuteName => T("Mute {0}", Label);

    [ObservableProperty]
    public partial bool IsMuted { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ValueText))]
    public partial double Value { get; set; }

    public string ValueText => Value.ToString("F" + decimals.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture) + unit;

    public override void Show(string value)
    {
        showing = true;
        Value = double.Parse(value, CultureInfo.InvariantCulture);
        showing = false;
    }

    /// <summary>Shows the mute key's value from the file without saving it.</summary>
    public void ShowMute(string value)
    {
        showing = true;
        IsMuted = value != "0";
        showing = false;
    }

    /// <summary>Back to neutral (the original), as the menu's reset icon.</summary>
    [RelayCommand]
    private void ResetValue() => Value = double.Parse(neutral, CultureInfo.InvariantCulture);

    partial void OnValueChanged(double value)
    {
        if (!showing)
        {
            owner.Changed(Key!, SettingsSchema.FormatNumber(Math.Round(value, decimals)));
        }
    }

    partial void OnIsMutedChanged(bool value)
    {
        if (!showing && MuteKey is not null)
        {
            owner.Changed(MuteKey, value ? "1" : "0");
        }
    }
}

/// <summary>One entry of the quality preset list: a preset, or "Custom" (Preset null); its name said in the language shown.</summary>
public sealed partial class PresetOption(Said name, QualityPreset? preset) : ObservableObject
{
    public string Name => name.ToString();

    public QualityPreset? Preset { get; } = preset;

    /// <summary>After a language switch: the name said again.</summary>
    public void SayAgain() => OnPropertyChanged(nameof(Name));
}

/// <summary>
/// The quality preset drop-down at the top of F2's Graphics tab (to-do 202): Low / Medium / High /
/// Ultra, "Custom" (added to the list only then) when one of its options was changed by hand.
/// </summary>
public sealed partial class PresetItem : SettingItem
{
    private readonly Action<QualityPreset> choose;
    private readonly PresetOption custom = new(S("Custom"), null);
    private bool showing;

    public PresetItem(MenuEntry entry, IReadOnlyList<QualityPreset> presets, Action<QualityPreset> choose)
        : base(entry)
    {
        this.choose = choose;
        foreach (QualityPreset p in presets)
        {
            Options.Add(new PresetOption(Said.Menu(p.Name), p));
        }
    }

    public override void SayAgain()
    {
        base.SayAgain();
        custom.SayAgain();
        foreach (PresetOption o in Options)
        {
            o.SayAgain();
        }
    }

    public ObservableCollection<PresetOption> Options { get; } = [];

    [ObservableProperty]
    public partial PresetOption? Selected { get; set; }

    public override void Show(string value)
    {
    }

    /// <summary>Shows the preset every value matches (values: the options' values now), else Custom.</summary>
    public void Update(IReadOnlyDictionary<string, string> values)
    {
        PresetOption match = Options.FirstOrDefault(o => o.Preset is { } p && p.Values.All(v =>
            values.TryGetValue(v.Key, out string? now) && now.Equals(v.Value, StringComparison.OrdinalIgnoreCase)))
            ?? custom;
        if (match == custom && !Options.Contains(custom))
        {
            Options.Add(custom);
        }

        if (!ReferenceEquals(Selected, match))
        {
            showing = true;
            Selected = match;
            showing = false;
        }

        if (match != custom)
        {
            Options.Remove(custom);
        }
    }

    partial void OnSelectedChanged(PresetOption? value)
    {
        if (!showing && value?.Preset is { } preset)
        {
            choose(preset);
        }
    }
}
