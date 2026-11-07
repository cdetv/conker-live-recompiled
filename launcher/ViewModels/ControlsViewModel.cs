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

    internal string KeyLabel(string name) =>
        schema?.KeyLabels.GetValueOrDefault(name) ?? name;

    internal void StartCapture(KeyItem item)
    {
        CaptureMessage = null;
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

    /// <summary>A key pressed while asking (the window sends every key): F3's rules for which are taken.</summary>
    public void TakeKey(Key key)
    {
        if (KeyInput.IsGameOwn(key))
        {
            CaptureMessage = "F1-F3, F9 and F11 are the game's own keys";
        }
        else if (KeyInput.Name(key) is { } name)
        {
            Take(name);
        }
        else
        {
            CaptureMessage = $"That key can't be used ({key})";
        }
    }

    /// <summary>A mouse button or wheel turn on the question (not on its buttons).</summary>
    public void TakeMouse(MouseButton button)
    {
        if (KeyInput.Name(button) is { } name)
        {
            Take(name);
        }
    }

    public void TakeWheel(double delta)
    {
        if (delta != 0)
        {
            Take(delta > 0 ? "wheelup" : "wheeldown");
        }
    }

    [RelayCommand]
    private void NoKey() => Take("none");

    [RelayCommand]
    private void CancelCapture() => Capturing = null;

    private void Take(string name)
    {
        if (Capturing is { } item)
        {
            Capturing = null;
            SetKey(item, name);
        }
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

            foreach (KeyItem k in keys)
            {
                k.ClashWith = !k.IsShown || k.Value == "none"
                    ? null
                    : keys.LastOrDefault(j => j != k && j.IsShown && j.Value == k.Value
                                              && (j.Section == k.Section || j.ClashesAll || k.ClashesAll)
                                              && j.Button != k.Button)?.Label;
            }
        }
    }
}
