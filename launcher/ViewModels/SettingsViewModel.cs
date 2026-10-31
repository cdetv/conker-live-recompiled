using System.Collections.ObjectModel;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// The Settings tab (SPEC 9.3): the F2 menu's entries, built from <c>conker.exe --dump-settings</c>.
/// A change is written to settings.ini shortly after it is made (a moving slider writes once it
/// stops), only the option's own line.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject, ISettingsOwner
{
    private readonly GameFolder folder;
    private readonly DecensorPatch decensor;
    private readonly Dictionary<string, string> pending = new(StringComparer.OrdinalIgnoreCase);
    private readonly DispatcherTimer saveTimer;
    private SettingsSchema? schema;

    public SettingsViewModel(GameFolder folder, IRelayCommand launchDebug)
    {
        this.folder = folder;
        decensor = new DecensorPatch(folder);
        LaunchDebugCommand = launchDebug;
        saveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
        saveTimer.Tick += (_, _) => Flush();
    }

    public ObservableCollection<SettingItem> Items { get; } = [];

    /// <summary>Starts the game with <c>--debug</c> (the main window's command).</summary>
    public IRelayCommand LaunchDebugCommand { get; }

    /// <summary>The game is running: its F2 menu rewrites settings.ini, so the tab is locked.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsEditable))]
    public partial bool IsLocked { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsEditable))]
    public partial bool IsLoaded { get; set; }

    public bool IsEditable => IsLoaded && !IsLocked;

    /// <summary>Why the settings can't be shown, or a write that failed.</summary>
    [ObservableProperty]
    public partial string? ErrorText { get; set; }

    /// <summary>Reset to defaults was pressed once: it asks before resetting.</summary>
    [ObservableProperty]
    public partial bool ConfirmReset { get; set; }

    /// <summary>Shows the options conker.exe described with the values in settings.ini (error: why it couldn't).</summary>
    public void Load(SettingsSchema? loaded, string? error)
    {
        if (loaded is null)
        {
            ErrorText = $"The settings couldn't be read from conker.exe: {error}";
            return;
        }

        schema = loaded;
        Items.Clear();
        foreach (MenuEntry entry in schema.Menu)
        {
            SettingItem? item = entry.Type switch
            {
                "section" => new SectionItem(entry),
                "checkbox" => new CheckItem(this, entry),
                "choice" => new ChoiceItem(this, entry),
                "slider" => new SliderItem(this, entry),
                _ => null,
            };
            if (item is not null)
            {
                if (item.Key is { } key && schema.ByKey.TryGetValue(key, out SettingOption? option))
                {
                    item.Tip ??= option.Help;
                }

                if (item.Parent is { } parent && Items.FirstOrDefault(i => i.Key == parent) is { } up)
                {
                    item.Depth = up.Depth + 1;
                }

                Items.Add(item);
            }
        }

        IsLoaded = true;
        Reload();
    }

    /// <summary>Shows what settings.ini holds now (after the game ran, F2 may have changed it).</summary>
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

        bool haveDecensor = decensor.IsInstalled;
        foreach (SettingItem item in Items)
        {
            if (item.Key is not { } key || !schema.ByKey.TryGetValue(key, out SettingOption? option))
            {
                continue;
            }

            // A value the game would ignore shows as the default it uses instead.
            string value = file.TryGetValue(key, out string? v) && schema.Normalize(option, v) is { } ok
                ? ok
                : option.Default;
            item.Show(value);
            item.IsAvailable = item.Needs != "decensor" || haveDecensor;
        }

        ShowChildren();
    }

    /// <summary>
    /// As in F2, a row under a checkbox (the Pop-in Fix's parts, the particle density under one of
    /// them) shows only while that checkbox is on and shown itself; parents come first in the menu.
    /// </summary>
    private void ShowChildren()
    {
        foreach (SettingItem item in Items)
        {
            if (item.Parent is { } parent && Items.OfType<CheckItem>().FirstOrDefault(c => c.Key == parent) is { } check)
            {
                item.IsShown = check.IsOn && check.IsShown;
            }
        }
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

    /// <summary>An item's new value, from the player: saved shortly.</summary>
    public void Changed(string key, string value)
    {
        pending[key] = value;
        saveTimer.Stop();
        saveTimer.Start();
        ShowChildren();
    }

    [RelayCommand]
    private void AskReset() => ConfirmReset = true;

    [RelayCommand]
    private void CancelReset() => ConfirmReset = false;

    /// <summary>Every option on this tab back to the game's default.</summary>
    [RelayCommand]
    private void Reset()
    {
        ConfirmReset = false;
        if (schema is null)
        {
            return;
        }

        foreach (SettingItem item in Items)
        {
            if (item.Key is { } key && schema.ByKey.TryGetValue(key, out SettingOption? option))
            {
                item.Show(option.Default);
                pending[key] = option.Default;
            }
        }

        ShowChildren();
        Flush();
    }
}
