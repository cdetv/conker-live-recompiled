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

/// <summary>One row of the Settings or Controls tab.</summary>
public abstract partial class SettingItem(MenuEntry entry) : ObservableObject
{
    public string Label { get; } = entry.Label;

    public string? Key { get; } = entry.Key;

    public string? Needs { get; } = entry.Needs;

    /// <summary>The menu's tooltip, else the option's settings.ini comment.</summary>
    public string? Tip { get; set; } = entry.Tip;

    /// <summary>False when what it needs (the decensor patch) isn't there.</summary>
    [ObservableProperty]
    public partial bool IsAvailable { get; set; } = true;

    /// <summary>The single player scheme the row belongs to; null for both.</summary>
    public string? Scheme { get; } = entry.Scheme;

    /// <summary>The checkbox this row sits under (indented, shown only while it is on); null for none.</summary>
    public string? Parent { get; } = entry.Parent;

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
}

public sealed class SectionItem(MenuEntry entry) : SettingItem(entry)
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

public sealed record ChoiceOption(string Value, string Name);

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
            Options.Add(new ChoiceOption(values[i], i < names.Count ? names[i] : values[i]));
        }
    }

    public ObservableCollection<ChoiceOption> Options { get; } = [];

    [ObservableProperty]
    public partial ChoiceOption? Selected { get; set; }

    public override void Show(string value)
    {
        // As in F2, a value only reachable by editing the file is listed too.
        ChoiceOption? match = Options.FirstOrDefault(o => o.Value.Equals(value, StringComparison.OrdinalIgnoreCase));
        if (match is null)
        {
            match = new ChoiceOption(value, value);
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

    public string? ResetTip { get; } = entry.ResetTip;

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
}
