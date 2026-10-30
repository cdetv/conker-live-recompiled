using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>The Files tab (SPEC 9.3): game files, the decensor patch, folder buttons.</summary>
public sealed partial class FilesViewModel : ObservableObject
{
    private readonly GameFolder folder;
    private readonly GameImporter importer;
    private readonly DecensorPatch decensor;
    private readonly IFilePicker picker;
    private CancellationTokenSource? job;

    public FilesViewModel(GameFolder folder, IFilePicker picker)
    {
        this.folder = folder;
        this.picker = picker;
        importer = new GameImporter(folder);
        decensor = new DecensorPatch(folder);
        try
        {
            importer.CleanLeftovers();
            decensor.CleanLeftovers();
        }
        catch (IOException)
        {
            // A leftover still in use; the next import removes it.
        }

        Refresh();
    }

    /// <summary>Raised when the game files or the busy state change (Play depends on both).</summary>
    public event EventHandler? StateChanged;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameFilesText), nameof(ImportButtonText))]
    public partial bool GameFilesImported { get; set; }

    public string GameFilesText => GameFilesImported
        ? "Game files have been imported"
        : "Conker Live & Reloaded files have not been imported yet. Import them to play";

    public string ImportButtonText => GameFilesImported ? "Re-import" : "Import Files";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DecensorText), nameof(DownloadButtonText), nameof(AddPatchButtonText))]
    public partial bool DecensorInstalled { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DownloadButtonText), nameof(DecensorText))]
    public partial ReleaseInfo? LatestDecensor { get; set; }

    [ObservableProperty]
    public partial string? DecensorCheckNote { get; set; }

    public string DecensorText => !DecensorInstalled
        ? "Decensor patch not found"
        : decensor.InstalledVersion is { } v ? $"Decensor Patch found (version {v})" : "Decensor Patch found";

    public string AddPatchButtonText => DecensorInstalled ? "Re-import" : "Add patch";

    public string DownloadButtonText =>
        !DecensorInstalled ? "Download patch"
        : LatestDecensor is { } latest && decensor.IsOlderThan(latest.Tag) ? $"Update to {latest.Tag}"
        : "Re-download";

    /// <summary>An import or download is running.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand))]
    [NotifyPropertyChangedFor(nameof(GameJobVisible), nameof(DecensorJobVisible))]
    public partial bool IsBusy { get; set; }

    /// <summary>The launcher is updating itself: no new job (the restart would cut it off).</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand))]
    public partial bool Updating { get; set; }

    [ObservableProperty]
    public partial string? ProgressText { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ProgressIndeterminate))]
    public partial double? ProgressFraction { get; set; }

    public bool ProgressIndeterminate => ProgressFraction is null;

    public double ProgressPercent => (ProgressFraction ?? 0) * 100;

    /// <summary>The last job's result, or why it failed.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameStatusVisible), nameof(DecensorStatusVisible))]
    public partial string? StatusText { get; set; }

    /// <summary>The running or last job was the decensor patch's (its progress and result show in that section).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameJobVisible), nameof(DecensorJobVisible), nameof(GameStatusVisible), nameof(DecensorStatusVisible))]
    public partial bool JobIsDecensor { get; set; }

    public bool GameJobVisible => IsBusy && !JobIsDecensor;

    public bool DecensorJobVisible => IsBusy && JobIsDecensor;

    public bool GameStatusVisible => StatusText is not null && !JobIsDecensor;

    public bool DecensorStatusVisible => StatusText is not null && JobIsDecensor;

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    /// <summary>Looks at the folder again.</summary>
    public void Refresh()
    {
        bool before = GameFilesImported;
        GameFilesImported = folder.GameFilesImported;
        DecensorInstalled = decensor.IsInstalled;
        OnPropertyChanged(nameof(DecensorText));
        OnPropertyChanged(nameof(DownloadButtonText));
        if (before != GameFilesImported)
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <summary>Asks GitHub for the decensor patch's latest release (once, at start-up).</summary>
    public async Task CheckLatestDecensorAsync()
    {
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            LatestDecensor = await GitHubReleases.GetLatestAsync(DecensorPatch.Repo, DecensorPatch.Extensions, timeout.Token);
            DecensorCheckNote = null;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException or KeyNotFoundException)
        {
            DecensorCheckNote = "GitHub couldn't be reached, so the launcher can't tell whether a newer patch is out.";
        }
    }

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task ImportGameAsync()
    {
        string? source = await picker.PickFileAsync(
            "Choose your Conker disc image, an archive holding it, or the default.xbe of a game folder",
            "Conker disc (.iso, .xiso, .zip, .7z, .rar, default.xbe)",
            [.. GameImporter.Extensions.Select(x => "*" + x)]);
        if (source is null)
        {
            return;
        }

        await RunJobAsync(false, cancel => Task.Run(() =>
        {
            string region = importer.Import(source, Progress(), cancel);
            return $"Game files imported ({region} disc).";
        }, cancel));
    }

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task AddPatchAsync()
    {
        string? source = await picker.PickFileAsync("Choose the decensor patch (.7z or .zip)", "Decensor patch (.7z, .zip)",
            [.. DecensorPatch.Extensions.Select(x => "*" + x)]);
        if (source is null)
        {
            return;
        }

        await RunJobAsync(true, cancel => Task.Run(() =>
        {
            decensor.Install(source, null, Progress(), cancel);
            return "Decensor patch added. Switch it on in the game's F2 menu (Uncensored).";
        }, cancel));
    }

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task DownloadPatchAsync()
    {
        await RunJobAsync(true, async cancel =>
        {
            ProgressText = "Looking for the latest release";
            ReleaseInfo latest = await GitHubReleases.GetLatestAsync(DecensorPatch.Repo, DecensorPatch.Extensions, cancel);
            LatestDecensor = latest;
            await decensor.DownloadAndInstallAsync(latest, Progress(), cancel);
            return $"Decensor patch {latest.Tag} installed. Switch it on in the game's F2 menu (Uncensored).";
        });
    }

    private bool CanStartJob() => !IsBusy && !Updating;

    [RelayCommand]
    private void Cancel() => job?.Cancel();

    [RelayCommand]
    private static void OpenLogs() => OpenFolder(SaveFolder.Logs);

    [RelayCommand]
    private static void OpenSaves() => OpenFolder(SaveFolder.Root);

    [RelayCommand]
    private static void OpenPhotos() => OpenFolder(SaveFolder.Photos);

    private static void OpenFolder(string path)
    {
        try
        {
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = false })?.Dispose();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            // Nothing to open; the folder appears once the game has written to it.
        }
    }

    private Progress<JobProgress> Progress() => new(p =>
    {
        ProgressText = p.Text;
        ProgressFraction = p.Fraction;
        OnPropertyChanged(nameof(ProgressPercent));
    });

    private async Task RunJobAsync(bool forDecensor, Func<CancellationToken, Task<string>> work)
    {
        using CancellationTokenSource cancel = new();
        job = cancel;
        JobIsDecensor = forDecensor;
        IsBusy = true;
        StatusText = null;
        ProgressText = "Starting";
        ProgressFraction = null;
        StateChanged?.Invoke(this, EventArgs.Empty);
        try
        {
            StatusText = await work(cancel.Token);
            StatusIsError = false;
        }
        catch (OperationCanceledException)
        {
            StatusText = "Cancelled. Nothing was changed.";
            StatusIsError = false;
        }
        catch (ImportException e)
        {
            StatusText = e.Message;
            StatusIsError = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or HttpRequestException
                                       or System.Text.Json.JsonException or KeyNotFoundException or InvalidOperationException)
        {
            StatusText = e is HttpRequestException ? $"The download failed: {e.Message}" : $"Something went wrong: {e.Message}";
            StatusIsError = true;
        }
        finally
        {
            job = null;
            IsBusy = false;
            Refresh();
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }
}
