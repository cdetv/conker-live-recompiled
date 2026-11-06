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
        // First: it decides where SaveFolder is, which the other tabs read.
        UserFiles = new UserFilesViewModel(folder, IsGameRunning);
        Files = new FilesViewModel(folder, picker, UserFiles);
        Settings = new SettingsViewModel(folder, LaunchDebugCommand);
        Controls = new ControlsViewModel(folder);
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
            }

            PlayCommand.NotifyCanExecuteChanged();
            LaunchDebugCommand.NotifyCanExecuteChanged();
            UpdateCommand.NotifyCanExecuteChanged();
            OpenUpdatePromptCommand.NotifyCanExecuteChanged();
        };
        Files.StateChanged += (_, _) =>
        {
            PlayCommand.NotifyCanExecuteChanged();
            LaunchDebugCommand.NotifyCanExecuteChanged();
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
    public partial string? HistoryNote { get; set; }

    public string UpdateTitle => NewRelease is { } r ? $"v{Updater.TagVersion(r.Tag)} is out (you have {VersionText})" : "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VersionLine))]
    public partial string? UpdateNote { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(UpdateCommand), nameof(PlayCommand), nameof(LaunchDebugCommand))]
    public partial bool IsUpdating { get; set; }

    public string UpdateButtonText => NewRelease is { } r ? $"Update to v{Updater.TagVersion(r.Tag)}" : "Update";

    public FilesViewModel Files { get; }

    /// <summary>Where the user files are kept; also the window's move and save-conflict pop-ups.</summary>
    public UserFilesViewModel UserFiles { get; }

    public SettingsViewModel Settings { get; }

    public ControlsViewModel Controls { get; }

    [ObservableProperty]
    public partial bool ShowImportPrompt { get; set; }

    /// <summary>A line at the bottom of the window, such as why the game didn't start.</summary>
    [ObservableProperty]
    public partial string? Message { get; set; }

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

        HistoryNote = "Loading the version history ...";
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            List<ReleaseInfo> releases = await Updater.GetHistoryAsync(timeout.Token);
            History = [.. releases.Select(r => new HistoryEntry(r))];
            HistoryNote = releases.Count == 0 ? "No releases found." : null;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            HistoryNote = $"Couldn't load the version history: {e.Message}";
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

    [RelayCommand(CanExecute = nameof(CanPlay))]
    private void Play() => StartGame(debug: false);

    /// <summary>Settings tab: the game with <c>--debug</c> (debug choices in its F2 menu).</summary>
    [RelayCommand(CanExecute = nameof(CanPlay))]
    private void LaunchDebug() => StartGame(debug: true);

    private void StartGame(bool debug)
    {
        if (!File.Exists(folder.GameExe))
        {
            Message = "conker.exe is missing from the launcher's folder.";
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

        try
        {
            Process.Start(start)?.Dispose();
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            Message = $"The game didn't start: {e.Message}";
            return;
        }

        CloseRequested?.Invoke(this, EventArgs.Empty);
    }

    private bool CanPlay() => Files.GameFilesImported && !Files.IsBusy && !IsUpdating && !UserFiles.IsBusy;

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
        }

        Settings.IsLocked = running;
        Controls.IsLocked = running;
        if (!running)
        {
            Settings.Reload();
            Controls.Reload();
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
        string? quiet = justUpdated ? "Updated" : null;
        UpdateNote = quiet;
        _ = updater.CleanAfterUpdateAsync();
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            ReleaseInfo latest = await Updater.GetLatestAsync(timeout.Token);
            NewRelease = Updater.IsNewer(latest) ? latest : null;
            UpdateNote = NewRelease is not null ? $"v{Updater.TagVersion(latest.Tag)} is out" : quiet;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException
                                       or KeyNotFoundException or InvalidOperationException)
        {
            // Also while the release repo doesn't exist yet (GitHub answers 404).
            UpdateNote = quiet ?? "Couldn't check for updates";
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
            Message = "Close the game first: the update replaces conker.exe.";
            return;
        }

        Flush();
        ShowUpdatePrompt = false;
        Message = null;
        IsUpdating = true;
        Files.Updating = true;
        var progress = new Progress<JobProgress>(p =>
            UpdateNote = p.Fraction is { } f ? $"{p.Text.Split(':')[0]}: {f:P0}" : p.Text);
        try
        {
            await updater.ApplyAsync(release, progress, CancellationToken.None);
            updater.Restart();
            CloseRequested?.Invoke(this, EventArgs.Empty);
            return;
        }
        catch (ImportException e)
        {
            Message = e.Message;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or HttpRequestException
                                       or System.ComponentModel.Win32Exception)
        {
            Message = e is HttpRequestException ? $"The update download failed: {e.Message}" : $"The update failed: {e.Message}";
        }

        UpdateNote = $"v{Updater.TagVersion(release.Tag)} is out";
        IsUpdating = false;
        Files.Updating = false;
    }

    private bool CanUpdate() => NewRelease is not null && !IsUpdating && !Files.IsBusy && !UserFiles.IsBusy;
}

/// <summary>A release in the Version History window.</summary>
public sealed class HistoryEntry(ReleaseInfo release)
{
    public string Title { get; } = $"v{Updater.TagVersion(release.Tag)?.ToString() ?? release.Tag}";

    /// <summary>"3 November 2026", and whether this launcher is that version.</summary>
    public string DateLine { get; } = string.Join(" · ", new[]
    {
        release.Published?.ToLocalTime().ToString("d MMMM yyyy", System.Globalization.CultureInfo.InvariantCulture),
        Updater.TagVersion(release.Tag) == Updater.Current ? "installed" : null,
    }.Where(s => s is not null));

    public string Body { get; } = release.Body;

    public Uri? Page { get; } = Uri.TryCreate(release.PageUrl, UriKind.Absolute, out Uri? u) ? u : null;
}
