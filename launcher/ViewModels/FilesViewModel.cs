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
    private readonly Func<bool> isGameRunning;
    private readonly PadInput pad;
    private CancellationTokenSource? job;

    public FilesViewModel(GameFolder folder, IFilePicker picker, UserFilesViewModel userFiles, Func<bool> isGameRunning, PadInput pad)
    {
        this.folder = folder;
        this.picker = picker;
        this.isGameRunning = isGameRunning;
        this.pad = pad;
        UserFiles = userFiles;
        Report = new ReportBugViewModel(folder, picker);
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

    /// <summary>The User files section: where they are kept.</summary>
    public UserFilesViewModel UserFiles { get; }

    /// <summary>The Report Bug pop-up (to-do 12d).</summary>
    public ReportBugViewModel Report { get; }

    /// <summary>Raised when the game files or the busy state change (Play depends on both).</summary>
    public event EventHandler? StateChanged;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameFilesText), nameof(ImportButtonText))]
    [NotifyCanExecuteChangedFor(nameof(PrecompileShadersCommand))]
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
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand), nameof(PrecompileShadersCommand))]
    [NotifyPropertyChangedFor(nameof(GameJobVisible), nameof(DecensorJobVisible))]
    public partial bool IsBusy { get; set; }

    /// <summary>The launcher is updating itself: no new job (the restart would cut it off).</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand), nameof(PrecompileShadersCommand))]
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

    /// <summary>A Precompile Shaders run started from here is still going (the game's window shows it).</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(PrecompileShadersCommand), nameof(AskDeleteShadersCommand), nameof(ImportGameCommand),
                                nameof(AddPatchCommand), nameof(DownloadPatchCommand))]
    [NotifyPropertyChangedFor(nameof(ShadersText), nameof(PrecompileButtonText))]
    public partial bool IsPrecompiling { get; set; }

    /// <summary>What the last run wrote; null when none ran.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShadersText), nameof(PrecompileButtonText))]
    public partial PrecompileResult? Precompiled { get; set; }

    /// <summary>The cache holds shaders or a result: Delete Shaders has something to delete.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskDeleteShadersCommand))]
    public partial bool CacheHasFiles { get; set; }

    /// <summary>Delete Shaders asked its question (Yes / No in its place).</summary>
    [ObservableProperty]
    public partial bool ConfirmDeleteShaders { get; set; }

    /// <summary>Why the run didn't start, or the delete failed.</summary>
    [ObservableProperty]
    public partial string? PrecompileError { get; set; }

    /// <summary>A stopped run of this version: the next one continues where it was.</summary>
    private bool CanContinue => Precompiled is { Finished: false, ResumeWhere: not null } r
                                && (r.Version is null || r.Version == Updater.Current);

    public string ShadersText =>
        IsPrecompiling ? "Precompiling shaders. The game's window shows how far it is; Esc or B on a controller there stops it."
        : Precompiled is not { } r
            ? "The game compiles each shader the first time it is needed, which can cause short slowdowns the first time "
              + "you play an area. Precompile them all now: the game runs every level behind a progress screen with "
              + "the sound off, then closes (about 10 minutes, longer on a slower PC)."
        : r.Version is { } v && v != Updater.Current
            ? $"Shaders were precompiled with v{v}. Updates can change the game's shaders: precompile again to have them all."
        : r.Finished ? $"Shaders precompiled on {r.Date} (took {Minutes(r.Seconds)})."
        : CanContinue
            ? $"Precompiling was stopped on {r.Date} at {r.ResumeWhere}. Continue Precompiling picks up where it stopped; "
              + "the shaders compiled until then are kept."
        : $"Precompiling was stopped on {r.Date}. The shaders compiled until then are kept; precompile again to finish.";

    public string PrecompileButtonText =>
        Precompiled is null ? "Precompile Shaders" : CanContinue ? "Continue Precompiling" : "Precompile Again";

    private static string Minutes(int seconds) => seconds < 90 ? $"{seconds} seconds" : $"{(seconds + 30) / 60} minutes";

    /// <summary>The main window's game watch: the game started or ended.</summary>
    public void GameRunningChanged()
    {
        PrecompileShadersCommand.NotifyCanExecuteChanged();
        AskDeleteShadersCommand.NotifyCanExecuteChanged();
        if (!IsPrecompiling)
        {
            Precompiled = ShaderCache.ReadResult();
            CacheHasFiles = ShaderCache.HasFiles();
        }
    }

    /// <summary>Looks at the folder again.</summary>
    public void Refresh()
    {
        bool before = GameFilesImported;
        GameFilesImported = folder.GameFilesImported;
        DecensorInstalled = decensor.IsInstalled;
        if (!IsPrecompiling)
        {
            Precompiled = ShaderCache.ReadResult();
            CacheHasFiles = ShaderCache.HasFiles();
        }

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

    /// <summary>What Import Files takes, in words (the file browser's and the Welcome pop-up's error lines).</summary>
    public const string GameFilterName = "Conker disc (.iso, .xiso, .zip, .7z, .rar, default.xbe)";

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task ImportGameAsync()
    {
        string? source = await picker.PickFileAsync(
            "Choose your Conker disc image, an archive holding it, or the default.xbe of a game folder",
            GameFilterName,
            [.. GameImporter.Extensions.Select(x => "*" + x)]);
        if (source is null)
        {
            return;
        }

        await ImportGameFromAsync(source);
    }

    /// <summary>
    /// Imports the game from this file: the one picked, or one dropped on the Welcome pop-up
    /// (to-do 122). False: another job runs (nothing started).
    /// </summary>
    public async Task<bool> ImportGameFromAsync(string source)
    {
        if (!CanStartJob())
        {
            return false;
        }

        await RunJobAsync(false, cancel => Task.Run(() =>
        {
            string region = importer.Import(source, Progress(), cancel);
            return $"Game files imported ({region} disc).";
        }, cancel));
        return true;
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

    private bool CanStartJob() => !IsBusy && !Updating && !IsPrecompiling;

    /// <summary>
    /// Precompile Shaders (to-do 25): <c>conker.exe --precompile-shaders</c> on the user files. The
    /// launcher stays open and shows the result when the game closes (src/precompile.c).
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanPrecompile))]
    private async Task PrecompileShadersAsync()
    {
        PrecompileError = null;
        if (!File.Exists(folder.GameExe))
        {
            PrecompileError = "conker.exe is missing from the launcher's folder.";
            return;
        }

        var start = new ProcessStartInfo(folder.GameExe) { WorkingDirectory = folder.Root, UseShellExecute = false };
        start.ArgumentList.Add("--save-dir");
        start.ArgumentList.Add(SaveFolder.Root);
        start.ArgumentList.Add("--precompile-shaders");
        // That run reads every controller (B stops it): the launcher's pad helper waits until it ends.
        pad.Stop();
        Process? game;
        try
        {
            game = Process.Start(start);
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            PrecompileError = $"The game didn't start: {e.Message}";
            pad.Start();
            return;
        }

        if (game is null)
        {
            pad.Start();
            return;
        }

        using (game)
        {
            IsPrecompiling = true;
            StateChanged?.Invoke(this, EventArgs.Empty);
            try
            {
                await game.WaitForExitAsync();
            }
            finally
            {
                IsPrecompiling = false;
                Precompiled = ShaderCache.ReadResult();
                CacheHasFiles = ShaderCache.HasFiles();
                pad.Start();
                StateChanged?.Invoke(this, EventArgs.Empty);
            }
        }
    }

    /// <summary>Delete Shaders: asks first (Yes / No in the button's place).</summary>
    [RelayCommand(CanExecute = nameof(CanDeleteShaders))]
    private void AskDeleteShaders()
    {
        PrecompileError = null;
        ConfirmDeleteShaders = true;
    }

    [RelayCommand]
    private void CancelDeleteShaders() => ConfirmDeleteShaders = false;

    /// <summary>Yes: every .vso / .cso and precompiled.ini in the cache go (ShaderCache.Delete).</summary>
    [RelayCommand]
    private void DeleteShaders()
    {
        ConfirmDeleteShaders = false;
        if (!CanDeleteShaders())
        {
            return;     // the game started meanwhile
        }

        try
        {
            ShaderCache.Delete();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            PrecompileError = $"Some shaders couldn't be deleted: {e.Message}";
        }

        Precompiled = ShaderCache.ReadResult();
        CacheHasFiles = ShaderCache.HasFiles();
    }

    private bool CanDeleteShaders() => CacheHasFiles && !IsPrecompiling && !UserFiles.IsBusy && !isGameRunning();

    private bool CanPrecompile() =>
        GameFilesImported && !IsBusy && !Updating && !IsPrecompiling && !UserFiles.IsBusy && !isGameRunning();

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
