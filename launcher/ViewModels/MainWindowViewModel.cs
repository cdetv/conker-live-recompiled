using System.Diagnostics;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>The launcher window (SPEC 9.3): tabs, Update and Play.</summary>
public sealed partial class MainWindowViewModel : ObservableObject
{
    private readonly GameFolder folder;
    private readonly Updater updater;
    private readonly DispatcherTimer gameWatch;

    public MainWindowViewModel(GameFolder folder, IFilePicker picker)
    {
        this.folder = folder;
        updater = new Updater(folder);
        Pad = new PadInput(folder);
        Hints = PadHints.For(PadHintState.Page, PadKind.Xbox);
        // First: it decides where SaveFolder is, which the other tabs read.
        UserFiles = new UserFilesViewModel(folder, IsGameRunning);
        // The launcher's language (to-do 289): the game's Menu language from settings.ini, before anything is shown.
        Lang.Init(folder.Root);
        Lang.FollowSettingsFile();
        Lang.Changed += (_, _) =>
        {
            if (History is { } history)
            {
                History = [.. history.Select(h => new HistoryEntry(h.Release))];
            }

            OnPropertyChanged(string.Empty);
        };
        // The schema: only for a pack list saved before there is a settings.ini (the new file's values).
        Files = new FilesViewModel(folder, picker, UserFiles, IsGameRunning, Pad, () => Settings?.Schema);
        Settings = new SettingsViewModel(folder);
        Controls = new ControlsViewModel(folder);
        Debug = new DebugViewModel(folder, IsGameRunning);
        UserFiles.StateChanged += (_, _) =>
        {
            if (UserFiles.IsBusy)
            {
                Flush();
            }
            else if (!Settings.IsLocked)
            {
                Settings.Reload();
                Controls.Reload();
                Debug.Load();
            }

            Files.PrecompileShadersCommand.NotifyCanExecuteChanged();
            Files.AskDeleteShadersCommand.NotifyCanExecuteChanged();
            Files.AskDeleteAssetsCommand.NotifyCanExecuteChanged();
            PlayCommand.NotifyCanExecuteChanged();
            UpdateCommand.NotifyCanExecuteChanged();
            OpenUpdatePromptCommand.NotifyCanExecuteChanged();
        };
        Files.StateChanged += (_, _) =>
        {
            UpdatePadAccepting();
            PlayCommand.NotifyCanExecuteChanged();
            UpdateCommand.NotifyCanExecuteChanged();
            OpenUpdatePromptCommand.NotifyCanExecuteChanged();
        };
        // First launch (and every start until the files are in): offer the import.
        ShowImportPrompt = !Files.GameFilesImported;
        LoadPrefs();

        // While the game runs, its F2/F3 menus rewrite settings.ini: Settings and Controls are locked.
        gameWatch = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1.5) };
        gameWatch.Tick += (_, _) => WatchGame();
        gameWatch.Start();
        WatchGame();
        UpdatePadAccepting();
    }

    /// <summary>Asks the window to close (Play closes the launcher).</summary>
    public event EventHandler? CloseRequested;

    public static string VersionText => $"v{Updater.Current}";

    /// <summary>The version, and what the update check found or an update is doing.</summary>
    public string VersionLine => UpdateNote is null ? VersionText : $"{VersionText} · {UpdateNote}";

    /// <summary>The latest release when it is newer than this launcher.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(UpdateCommand), nameof(OpenUpdatePromptCommand))]
    [NotifyPropertyChangedFor(nameof(UpdateButtonText), nameof(UpdateTitle))]
    public partial ReleaseInfo? NewRelease { get; set; }

    /// <summary>The update window (SPEC 9.3): at start for a new release, and from the Update button.</summary>
    [ObservableProperty]
    public partial bool ShowUpdatePrompt { get; set; }

    /// <summary>"Don't remind me": no update window at start until a newer release than this one.</summary>
    [ObservableProperty]
    public partial bool SkipThisRelease { get; set; }

    /// <summary>Bottom bar: install a new release at start without asking.</summary>
    [ObservableProperty]
    public partial bool AutoUpdate { get; set; }

    [ObservableProperty]
    public partial bool ShowHistory { get; set; }

    /// <summary>Every release for the Version History window, newest first (loaded once).</summary>
    [ObservableProperty]
    public partial IReadOnlyList<HistoryEntry>? History { get; set; }

    /// <summary>"Loading ..." or why the history couldn't be loaded.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HistoryNote))]
    public partial Said? HistoryNoteSaid { get; set; }

    public string? HistoryNote => HistoryNoteSaid?.ToString();

    public string UpdateTitle => NewRelease is { } r ? T("v{0} is out (you have {1})", Updater.TagVersion(r.Tag), VersionText) : "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VersionLine), nameof(UpdateNote))]
    public partial Said? UpdateNoteSaid { get; set; }

    public string? UpdateNote => UpdateNoteSaid?.ToString();

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(UpdateCommand), nameof(PlayCommand))]
    public partial bool IsUpdating { get; set; }

    public string UpdateButtonText => NewRelease is { } r ? T("Update to v{0}", Updater.TagVersion(r.Tag)) : T("Update");

    public FilesViewModel Files { get; }

    /// <summary>Where the user files are kept; also the window's move and save-conflict pop-ups.</summary>
    public UserFilesViewModel UserFiles { get; }

    public SettingsViewModel Settings { get; }

    public ControlsViewModel Controls { get; }

    /// <summary>The Debug tab (to-do 144): Debug Mode and the save editor.</summary>
    public DebugViewModel Debug { get; }

    /// <summary>A drag hovering the Welcome pop-up that it won't take: why (in place of its hint line; to-do 122).</summary>
    [ObservableProperty]
    public partial string? WelcomeDropProblem { get; set; }

    /// <summary>A disc image (or game folder's <c>default.xbe</c>) dropped on the Welcome pop-up: imported at once, as after Import Files.</summary>
    public async Task ImportDroppedAsync(string source)
    {
        WelcomeDropProblem = null;
        ShowImportPrompt = false;
        await Files.ImportGameFromAsync(source);
    }

    /// <summary>The launcher's own folder: where the file browser opens (to-do 122).</summary>
    public string LauncherFolder => folder.Root;

    /// <summary>The file browser showing (to-do 122), or null; <c>MainWindow.PickFileAsync</c> opens and closes it.</summary>
    [ObservableProperty]
    public partial FileBrowserViewModel? Browser { get; set; }

    /// <summary>The controllers (to-do 121): the <c>conker.exe --pad-feed</c> helper.</summary>
    public PadInput Pad { get; }

    /// <summary>
    /// The pad is in use (to-do 121 part 1f): the help line and the button hints show. On with
    /// a pad press (PadNavigator), off when the mouse moves or clicks (MainWindow).
    /// </summary>
    [ObservableProperty]
    public partial bool PadMode { get; set; }

    /// <summary>The focused control's tooltip for the help line (a pad can't hover); MainWindow keeps it.</summary>
    [ObservableProperty]
    public partial string? HelpText { get; set; }

    /// <summary>The button hints bar (PadNavigator, by what has the pad and the pad's type).</summary>
    [ObservableProperty]
    public partial IReadOnlyList<PadHint> Hints { get; set; }

    /// <summary>The window is the active one (the pad drives it only then).</summary>
    public bool WindowActive
    {
        get;
        set
        {
            field = value;
            UpdatePadAccepting();
        }
    }

    /// <summary>
    /// The pad drives the launcher only while its window is in front (any window with the test
    /// file) and no game, precompile run or update is going.
    /// </summary>
    private void UpdatePadAccepting() =>
        Pad.Accepting = (WindowActive || Pad.FromFile) && !Settings.IsLocked && !Files.IsPrecompiling && !IsUpdating;

    partial void OnIsUpdatingChanged(bool value) => UpdatePadAccepting();

    [ObservableProperty]
    public partial bool ShowImportPrompt { get; set; }

    /// <summary>A line at the bottom of the window, such as why the game didn't start.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Message))]
    public partial Said? MessageSaid { get; set; }

    public string? Message => MessageSaid?.ToString();

    /// <summary>Looks at the folder again (the window calls this whenever it is activated).</summary>
    public void Refresh()
    {
        if (!Files.IsBusy)
        {
            Files.Refresh();
        }

        if (!Settings.IsLocked)
        {
            Settings.Reload();
            Controls.Reload();
        }

        Debug.Load();
    }

    /// <summary>
    /// Asks conker.exe for its F2/F3 menus (<c>--dump-settings</c>, once for both tabs) and its
    /// controllers (<c>--list-pads</c>).
    /// </summary>
    public async Task LoadAsync()
    {
        SettingsSchema? schema = null;
        string? error = null;
        try
        {
            schema = await SettingsSchema.LoadAsync(folder);
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception
                                       or System.Text.Json.JsonException or OperationCanceledException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            error = e.Message;
        }

        Settings.Load(schema, error);
        Controls.Load(schema, error);
        if (schema is not null)
        {
            await Controls.RefreshPadsAsync();
        }
    }

    /// <summary>Writes what the tabs haven't saved yet.</summary>
    public void Flush()
    {
        Settings.Flush();
        Controls.Flush();
    }

    [RelayCommand]
    private void DismissImportPrompt() => ShowImportPrompt = false;

    // While the window reads launcher.ini, the checkboxes don't write it back.
    private bool loadingPrefs;

    /// <summary>Reads launcher.ini (again once the user files are in place).</summary>
    private void LoadPrefs()
    {
        loadingPrefs = true;
        AutoUpdate = LauncherPrefs.AutoUpdate;
        SkipThisRelease = NewRelease is { } r && LauncherPrefs.SkippedUpdate == Updater.TagVersion(r.Tag);
        loadingPrefs = false;
    }

    partial void OnAutoUpdateChanged(bool value)
    {
        if (!loadingPrefs)
        {
            LauncherPrefs.AutoUpdate = value;
        }
    }

    partial void OnSkipThisReleaseChanged(bool value)
    {
        if (!loadingPrefs && NewRelease is { } r)
        {
            LauncherPrefs.SkippedUpdate = value ? Updater.TagVersion(r.Tag) : null;
        }
    }

    /// <summary>The Update button: the update window, whatever "Don't remind me" says.</summary>
    [RelayCommand(CanExecute = nameof(CanUpdate))]
    private void OpenUpdatePrompt()
    {
        ShowHistory = false;
        ShowUpdatePrompt = true;
    }

    [RelayCommand]
    private void UpdateLater() => ShowUpdatePrompt = false;

    /// <summary>Version History: every release's page text, from GitHub (once a launcher start).</summary>
    [RelayCommand]
    private async Task OpenHistoryAsync()
    {
        ShowUpdatePrompt = false;
        ShowHistory = true;
        if (History is not null)
        {
            return;
        }

        HistoryNoteSaid = S("Loading the version history ...");
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            List<ReleaseInfo> releases = await Updater.GetHistoryAsync(timeout.Token);
            History = [.. releases.Select(r => new HistoryEntry(r))];
            HistoryNoteSaid = releases.Count == 0 ? S("No releases found.") : null;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            HistoryNoteSaid = S("Couldn't load the version history: {0}", Why(e));
        }
    }

    [RelayCommand]
    private void CloseHistory() => ShowHistory = false;

    [RelayCommand]
    private async Task ImportFromPromptAsync()
    {
        ShowImportPrompt = false;
        await Files.ImportGameCommand.ExecuteAsync(null);
    }

    /// <summary>The Welcome pop-up's Import Xbox Store version (shown when one is installed; to-do 37).</summary>
    [RelayCommand]
    private async Task ImportStoreFromPromptAsync()
    {
        ShowImportPrompt = false;
        if (Files.ImportStoreVersionCommand.CanExecute(null))
        {
            await Files.ImportStoreVersionCommand.ExecuteAsync(null);
        }
    }

    /// <summary>Play: with <c>--debug</c> when the Debug tab's Debug Mode is on (to-do 144).</summary>
    [RelayCommand(CanExecute = nameof(CanPlay))]
    private void Play() => StartGame(debug: Debug.DebugMode);

    private void StartGame(bool debug)
    {
        if (!File.Exists(folder.GameExe))
        {
            MessageSaid = S("conker.exe is missing from the launcher's folder.");
            return;
        }

        Flush();
        var start = new ProcessStartInfo(folder.GameExe) { WorkingDirectory = folder.Root, UseShellExecute = false };
        // Always: the launcher decides where the user files are (SPEC 9.1).
        start.ArgumentList.Add("--save-dir");
        start.ArgumentList.Add(SaveFolder.Root);

        if (debug)
        {
            start.ArgumentList.Add("--debug");
        }

        // The game reads the pads itself; the launcher closes now anyway.
        Pad.Stop();
        try
        {
            Process.Start(start)?.Dispose();
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            MessageSaid = S("The game didn't start: {0}", e.Message);
            Pad.Start();
            return;
        }

        CloseRequested?.Invoke(this, EventArgs.Empty);
    }

    private bool CanPlay() => Files.GameFilesImported && !Files.IsBusy && !Files.IsPrecompiling && !IsUpdating && !UserFiles.IsBusy;

    /// <summary>Locks Settings and Controls while this folder's conker.exe runs; after it ends, shows what F2/F3 left.</summary>
    private void WatchGame()
    {
        bool running = IsGameRunning();
        if (running == Settings.IsLocked)
        {
            return;
        }

        if (running)
        {
            Flush();
            Controls.Capturing = null;
            // Also a game started outside the launcher: one program reads the pads at a time.
            Pad.Stop();
        }
        else if (!IsUpdating && !Files.IsPrecompiling)
        {
            Pad.Start();
        }

        Settings.IsLocked = running;
        Controls.IsLocked = running;
        UpdatePadAccepting();
        Files.GameRunningChanged();
        if (!running)
        {
            Settings.Reload();
            Controls.Reload();
            Debug.Load();
            // A copied-in folder or another version may have left files in the other place.
            _ = UserFiles.MoveInAsync();
        }
    }

    private bool IsGameRunning()
    {
        string exe = Path.GetFullPath(folder.GameExe);
        bool found = false;
        foreach (Process p in Process.GetProcessesByName("conker"))
        {
            using (p)
            {
                if (OwnProcesses.Contains(p.Id))
                {
                    continue;       // the launcher's own runs (pad helper, --list-pads ...), not the game
                }

                try
                {
                    found |= string.Equals(p.MainModule?.FileName, exe, StringComparison.OrdinalIgnoreCase);
                }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException)
                {
                    // Gone, or not ours to look at.
                }
            }
        }

        return found;
    }

    /// <summary>
    /// Asks GitHub for our latest release (once, at start-up). Also deletes what the last
    /// update left; <paramref name="justUpdated"/> (started by an update) says it happened.
    /// </summary>
    public async Task CheckForUpdateAsync(bool justUpdated)
    {
        Said? quiet = justUpdated ? S("Updated") : null;
        UpdateNoteSaid = quiet;
        _ = updater.CleanAfterUpdateAsync();
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            ReleaseInfo latest = await Updater.GetLatestAsync(timeout.Token);
            NewRelease = Updater.IsNewer(latest) ? latest : null;
            UpdateNoteSaid = NewRelease is not null ? S("v{0} is out", Updater.TagVersion(latest.Tag)) : quiet;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            // Also while the release repo doesn't exist yet (GitHub answers 404).
            UpdateNoteSaid = quiet ?? S("Couldn't check for updates");
        }

        LoadPrefs();
        if (NewRelease is null)
        {
            return;
        }

        if (AutoUpdate && CanUpdate())
        {
            await UpdateCommand.ExecuteAsync(null);
        }
        else if (!SkipThisRelease)
        {
            ShowUpdatePrompt = true;
        }
    }

    /// <summary>Downloads the new release, replaces the program files and restarts the launcher.</summary>
    [RelayCommand(CanExecute = nameof(CanUpdate))]
    private async Task UpdateAsync()
    {
        if (NewRelease is not { } release)
        {
            return;
        }

        if (IsGameRunning())
        {
            MessageSaid = S("Close the game first: the update replaces conker.exe.");
            return;
        }

        Flush();
        ShowUpdatePrompt = false;
        MessageSaid = null;
        IsUpdating = true;
        Files.Updating = true;
        // The update replaces conker.exe, which the pad helper runs.
        Pad.Stop();
        // The bottom line is short: the download as a percentage, the other steps by their words.
        var progress = new Progress<JobProgress>(p =>
            UpdateNoteSaid = p.Fraction is { } f ? S("Downloading v{0}: {1:P0}", Updater.TagVersion(release.Tag), f) : p.Text);
        try
        {
            await updater.ApplyAsync(release, progress, CancellationToken.None);
            updater.Restart();
            CloseRequested?.Invoke(this, EventArgs.Empty);
            return;
        }
        catch (ImportException e)
        {
            MessageSaid = e.Text;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or HttpRequestException
                                       or System.ComponentModel.Win32Exception)
        {
            MessageSaid = e is HttpRequestException ? S("The update download failed: {0}", Why(e)) : S("The update failed: {0}", Why(e));
        }

        UpdateNoteSaid = S("v{0} is out", Updater.TagVersion(release.Tag));
        IsUpdating = false;
        Files.Updating = false;
        Pad.Start();
    }

    private bool CanUpdate() => NewRelease is not null && !IsUpdating && !Files.IsBusy && !Files.IsPrecompiling && !UserFiles.IsBusy;
}

/// <summary>A release in the Version History window (made again after a language switch: its date line is words).</summary>
public sealed class HistoryEntry(ReleaseInfo release)
{
    public ReleaseInfo Release { get; } = release;

    public string Title { get; } = $"v{Updater.TagVersion(release.Tag)?.ToString() ?? release.Tag}";

    /// <summary>"3 November 2026" (the language's date), and whether this launcher is that version.</summary>
    public string DateLine { get; } = string.Join(" · ", new[]
    {
        release.Published is { } p ? Lang.Day(p.ToLocalTime().DateTime) : null,
        Updater.TagVersion(release.Tag) == Updater.Current ? T("installed") : null,
    }.Where(s => s is not null));

    public string Body { get; } = release.Body;

    public Uri? Page { get; } = Uri.TryCreate(release.PageUrl, UriKind.Absolute, out Uri? u) ? u : null;
}
