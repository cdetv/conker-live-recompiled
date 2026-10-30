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
        Files = new FilesViewModel(folder, picker);
        Settings = new SettingsViewModel(folder, LaunchDebugCommand);
        Controls = new ControlsViewModel(folder);
        Files.StateChanged += (_, _) =>
        {
            PlayCommand.NotifyCanExecuteChanged();
            LaunchDebugCommand.NotifyCanExecuteChanged();
            UpdateCommand.NotifyCanExecuteChanged();
        };
        // First launch (and every start until the files are in): offer the import.
        ShowImportPrompt = !Files.GameFilesImported;

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
    [NotifyCanExecuteChangedFor(nameof(UpdateCommand))]
    [NotifyPropertyChangedFor(nameof(UpdateButtonText))]
    public partial ReleaseInfo? NewRelease { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VersionLine))]
    public partial string? UpdateNote { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(UpdateCommand), nameof(PlayCommand), nameof(LaunchDebugCommand))]
    public partial bool IsUpdating { get; set; }

    public string UpdateButtonText => NewRelease is { } r ? $"Update to v{Updater.TagVersion(r.Tag)}" : "Update";

    public FilesViewModel Files { get; }

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
        if (SaveFolder.Override is { } saveDir)
        {
            start.ArgumentList.Add("--save-dir");
            start.ArgumentList.Add(saveDir);
        }

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

    private bool CanPlay() => Files.GameFilesImported && !Files.IsBusy && !IsUpdating;

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

    private bool CanUpdate() => NewRelease is not null && !IsUpdating && !Files.IsBusy;
}
