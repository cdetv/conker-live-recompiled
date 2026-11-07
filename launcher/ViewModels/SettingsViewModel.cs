using System.Collections.ObjectModel;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// The Settings tab (SPEC 9.3): the F2 menu's entries and tabs (Display, Picture, Sound, Recomp),
/// built from <c>conker.exe --dump-settings</c>. A change is written to settings.ini shortly after it is made (a moving slider writes once it
/// stops), only the option's own line.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject, ISettingsOwner
{
    private readonly GameFolder folder;
    private readonly DecensorPatch decensor;
    private readonly Dictionary<string, string> pending = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<SettingItem> items = [];
    private bool haveDecensor;
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

    /// <summary>What comes before F2's tabs (nothing today; everything, from a game without tabs).</summary>
    public ObservableCollection<SettingItem> TopItems { get; } = [];

    public ObservableCollection<MenuTab> Tabs { get; } = [];

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
        TopItems.Clear();
        Tabs.Clear();
        items.Clear();
        MenuTab? tab = null;
        foreach (MenuEntry entry in schema.Menu)
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
                "line" => new LineItem(entry),
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

                if (item.Parent is { } parent && items.FirstOrDefault(i => i.Key == parent) is { } up)
                {
                    item.Depth = up.Depth + 1;
                }

                items.Add(item);
                (tab?.Items ?? TopItems).Add(item);
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

        haveDecensor = decensor.IsInstalled;
        foreach (SettingItem item in items)
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
            if (item is SliderItem { MuteKey: { } muteKey } slider && schema.ByKey.TryGetValue(muteKey, out SettingOption? mute))
            {
                slider.ShowMute(file.TryGetValue(muteKey, out string? m) && schema.Normalize(mute, m) is { } okMute
                    ? okMute
                    : mute.Default);
            }
        }

        ShowChildren();
    }

    /// <summary>
    /// As in F2, a row under a checkbox (the Pop-in Fix's parts, the particle density under one of
    /// them) shows only while that checkbox is on and shown itself; parents come first in the menu.
    /// A row that needs something is greyed out without it: the decensor patch, or a checkbox on
    /// (Poo Fix needs Underwater Shimmer, on another tab).
    /// </summary>
    private void ShowChildren()
    {
        foreach (SettingItem item in items)
        {
            item.IsAvailable = item.Needs switch
            {
                null => true,
                "decensor" => haveDecensor,
                string needs => items.OfType<CheckItem>().FirstOrDefault(c => c.Key == needs)?.IsOn ?? true,
            };

            if (item.Parent is { } parent && items.OfType<CheckItem>().FirstOrDefault(c => c.Key == parent) is { } check)
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

        foreach (SettingItem item in items)
        {
            if (item.Key is { } key && schema.ByKey.TryGetValue(key, out SettingOption? option))
            {
                item.Show(option.Default);
                pending[key] = option.Default;
            }

            if (item is SliderItem { MuteKey: { } muteKey } slider && schema.ByKey.TryGetValue(muteKey, out SettingOption? mute))
            {
                slider.ShowMute(mute.Default);
                pending[muteKey] = mute.Default;
            }
        }

        ShowChildren();
        Flush();
    }
}
