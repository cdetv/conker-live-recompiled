using System.Diagnostics;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>Which section a Files tab job shows its progress and result in.</summary>
public enum FilesJob
{
    Game,
    Decensor,
    Textures,
}

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

    public FilesViewModel(GameFolder folder, IFilePicker picker, UserFilesViewModel userFiles, Func<bool> isGameRunning, PadInput pad,
                          Func<SettingsSchema?> schema)
    {
        this.folder = folder;
        this.picker = picker;
        this.isGameRunning = isGameRunning;
        this.pad = pad;
        UserFiles = userFiles;
        Packs = new TexturePacksViewModel(folder, isGameRunning, schema) { Log = pad.Log };
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
        // A language switch (to-do 289): every text here said again.
        Lang.Changed += (_, _) => OnPropertyChanged(string.Empty);
    }

    /// <summary>The User files section: where they are kept.</summary>
    public UserFilesViewModel UserFiles { get; }

    /// <summary>The Report Bug pop-up (to-do 12d).</summary>
    public ReportBugViewModel Report { get; }

    /// <summary>The Texture packs section's list (to-do 288 part 2d).</summary>
    public TexturePacksViewModel Packs { get; }

    /// <summary>The Steam section: Add to Steam (to-do 285; real Windows only).</summary>
    public SteamViewModel Steam { get; } = new();

    /// <summary>Raised when the game files or the busy state change (Play depends on both).</summary>
    public event EventHandler? StateChanged;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameFilesText), nameof(ImportButtonText))]
    [NotifyCanExecuteChangedFor(nameof(PrecompileShadersCommand), nameof(ExportTexturesCommand))]
    public partial bool GameFilesImported { get; set; }

    public string GameFilesText => GameFilesImported
        ? T("Game files have been imported")
        : T("Conker Live & Reloaded files have not been imported yet. Import them to play");

    public string ImportButtonText => GameFilesImported ? T("Re-import") : T("Import Files");

    /// <summary>
    /// The installed Xbox Store version found on this PC (its <c>DefaultPackage</c>), or null
    /// (to-do 37): the Files tab and the Welcome pop-up show Import Xbox Store version then.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StoreVersionFound))]
    [NotifyCanExecuteChangedFor(nameof(ImportStoreVersionCommand))]
    public partial string? FoundStorePackage { get; set; }

    public bool StoreVersionFound => FoundStorePackage is not null;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DecensorText), nameof(DownloadButtonText), nameof(AddPatchButtonText))]
    public partial bool DecensorInstalled { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DownloadButtonText), nameof(DecensorText))]
    public partial ReleaseInfo? LatestDecensor { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DecensorCheckNote))]
    public partial Said? DecensorCheckSaid { get; set; }

    public string? DecensorCheckNote => DecensorCheckSaid?.ToString();

    public string DecensorText => !DecensorInstalled
        ? T("Decensor patch not found")
        : decensor.InstalledVersion is { } v ? T("Decensor Patch found (version {0})", v) : T("Decensor Patch found");

    public string AddPatchButtonText => DecensorInstalled ? T("Re-import") : T("Add patch");

    public string DownloadButtonText =>
        !DecensorInstalled ? T("Download patch")
        : LatestDecensor is { } latest && decensor.IsOlderThan(latest.Tag) ? T("Update to {0}", latest.Tag)
        : T("Re-download");

    /// <summary>An import or download is running.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(ImportStoreVersionCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand),
                                nameof(PrecompileShadersCommand), nameof(AskDeleteAssetsCommand), nameof(ExportTexturesCommand))]
    [NotifyPropertyChangedFor(nameof(GameJobVisible), nameof(DecensorJobVisible), nameof(TexturesJobVisible))]
    public partial bool IsBusy { get; set; }

    /// <summary>The launcher is updating itself: no new job (the restart would cut it off).</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ImportGameCommand), nameof(ImportStoreVersionCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand), nameof(PrecompileShadersCommand),
                                nameof(ExportTexturesCommand))]
    public partial bool Updating { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ProgressText))]
    public partial Said? ProgressSaid { get; set; }

    public string? ProgressText => ProgressSaid?.ToString();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ProgressIndeterminate))]
    public partial double? ProgressFraction { get; set; }

    public bool ProgressIndeterminate => ProgressFraction is null;

    public double ProgressPercent => (ProgressFraction ?? 0) * 100;

    /// <summary>The last job's result, or why it failed.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusText), nameof(GameStatusVisible), nameof(DecensorStatusVisible), nameof(TexturesStatusVisible))]
    public partial Said? StatusSaid { get; set; }

    public string? StatusText => StatusSaid?.ToString();

    /// <summary>The section the running or last job belongs to (its progress and result show there).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GameJobVisible), nameof(DecensorJobVisible), nameof(TexturesJobVisible),
                              nameof(GameStatusVisible), nameof(DecensorStatusVisible), nameof(TexturesStatusVisible))]
    public partial FilesJob JobKind { get; set; }

    public bool GameJobVisible => IsBusy && JobKind == FilesJob.Game;

    public bool DecensorJobVisible => IsBusy && JobKind == FilesJob.Decensor;

    public bool TexturesJobVisible => IsBusy && JobKind == FilesJob.Textures;

    public bool GameStatusVisible => StatusSaid is not null && JobKind == FilesJob.Game;

    public bool DecensorStatusVisible => StatusSaid is not null && JobKind == FilesJob.Decensor;

    public bool TexturesStatusVisible => StatusSaid is not null && JobKind == FilesJob.Textures;

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    /// <summary>A Precompile Shaders run started from here is still going (the game's window shows it).</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(PrecompileShadersCommand), nameof(AskDeleteShadersCommand), nameof(ImportGameCommand),
                                nameof(ImportStoreVersionCommand), nameof(AddPatchCommand), nameof(DownloadPatchCommand),
                                nameof(ExportTexturesCommand))]
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
    [NotifyPropertyChangedFor(nameof(PrecompileError))]
    public partial Said? PrecompileErrorSaid { get; set; }

    public string? PrecompileError => PrecompileErrorSaid?.ToString();

    /// <summary>A stopped run of this version: the next one continues where it was.</summary>
    private bool CanContinue => Precompiled is { Finished: false, ResumeWhere: not null } r
                                && (r.Version is null || r.Version == Updater.Current);

    public string ShadersText =>
        IsPrecompiling ? T("Precompiling shaders. The game's window shows how far it is; Esc or B on a controller there stops it.")
        : Precompiled is not { } r
            ? T("The game compiles each shader the first time it is needed, which can cause short slowdowns the first time "
                + "you play an area. Precompile them all now: the game runs every level behind a progress screen with "
                + "the sound off, then closes (about 10 minutes, longer on a slower PC).")
        : r.Version is { } v && v != Updater.Current
            ? T("Shaders were precompiled with v{0}. Updates can change the game's shaders: precompile again to have them all.", v)
        : r.Finished ? T("Shaders precompiled on {0} (took {1}).", r.Date, Minutes(r.Seconds))
        : CanContinue
            ? T("Precompiling was stopped on {0} at {1}. Continue Precompiling picks up where it stopped; "
                + "the shaders compiled until then are kept.", r.Date, r.ResumeWhere)
        : T("Precompiling was stopped on {0}. The shaders compiled until then are kept; precompile again to finish.", r.Date);

    public string PrecompileButtonText =>
        Precompiled is null ? T("Precompile Shaders") : CanContinue ? T("Continue Precompiling") : T("Precompile Again");

    private static string Minutes(int seconds) => seconds < 90 ? T("{0} seconds", seconds) : T("{0} minutes", (seconds + 30) / 60);

    /// <summary>The main window's game watch: the game started or ended.</summary>
    public void GameRunningChanged()
    {
        PrecompileShadersCommand.NotifyCanExecuteChanged();
        AskDeleteShadersCommand.NotifyCanExecuteChanged();
        AskDeleteAssetsCommand.NotifyCanExecuteChanged();
        if (!IsPrecompiling)
        {
            Precompiled = ShaderCache.ReadResult();
            CacheHasFiles = ShaderCache.HasFiles();
        }

        // Locked while it runs; after it, what its F2 menu left.
        Packs.Refresh();
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
        RefreshAssets();
        RefreshTextures();
        Packs.Refresh();                // a pack folder put in while the launcher was behind
        if (before != GameFilesImported)
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
        }

        _ = FindStoreVersionAsync();
    }

    private bool findingStore;

    /// <summary>
    /// Looks for an installed Xbox Store version (to-do 37) off the UI thread (a sleeping drive
    /// can take seconds); again whenever the window comes to the front, so a copy installed
    /// meanwhile shows up. <c>CONKER_LAUNCHER_STORE_ROOTS</c> (development only): the drive
    /// tops to search instead of the fixed drives, separated by <c>;</c> (a missing folder = none found).
    /// </summary>
    private async Task FindStoreVersionAsync()
    {
        if (findingStore)
        {
            return;
        }

        findingStore = true;
        try
        {
            string? roots = Environment.GetEnvironmentVariable("CONKER_LAUNCHER_STORE_ROOTS");
            List<string> found = await Task.Run(() => roots is { Length: > 0 }
                ? StorePackage.FindInstalled(roots.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                : StorePackage.FindInstalled());

            // Bindings read it: set it on the UI thread, whatever thread the await came back on
            // (the first search starts while the window is still being built).
            if (Dispatcher.UIThread.CheckAccess())
            {
                FoundStorePackage = found.FirstOrDefault();
            }
            else
            {
                await Dispatcher.UIThread.InvokeAsync(() => FoundStorePackage = found.FirstOrDefault());
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            // The drive list couldn't be read: nothing offered (Import Files still takes the package).
        }
        finally
        {
            findingStore = false;
        }
    }

    /// <summary>Import Xbox Store version: the found install, imported at once (no file browser).</summary>
    [RelayCommand(CanExecute = nameof(CanImportStoreVersion))]
    private Task ImportStoreVersionAsync() => FoundStorePackage is { } package ? ImportGameFromAsync(package) : Task.CompletedTask;

    private bool CanImportStoreVersion() => StoreVersionFound && CanStartJob();

    /// <summary>Asks GitHub for the decensor patch's latest release (once, at start-up).</summary>
    public async Task CheckLatestDecensorAsync()
    {
        try
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(15));
            LatestDecensor = await GitHubReleases.GetLatestAsync(DecensorPatch.Repo, DecensorPatch.Extensions, timeout.Token);
            DecensorCheckSaid = null;
        }
        catch (Exception e) when (e is HttpRequestException or OperationCanceledException or System.Text.Json.JsonException or KeyNotFoundException)
        {
            DecensorCheckSaid = S("GitHub couldn't be reached, so the launcher can't tell whether a newer patch is out.");
        }
    }

    /// <summary>What Import Files takes, in words (the file browser's and the Welcome pop-up's error lines).</summary>
    public static string GameFilterName => T("Conker disc (.iso, .xiso, .zip, .7z, .rar, default.xbe) or Xbox Store version (DefaultPackage)");

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task ImportGameAsync()
    {
        string? source = await picker.PickFileAsync(
            T("Choose your Conker disc image, an archive holding it, the default.xbe of a game folder, or the Xbox Store version's DefaultPackage"),
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

        await RunJobAsync(FilesJob.Game, async cancel =>
        {
            Said what = await Task.Run(() => importer.Import(source, Progress(), cancel), cancel);
            return S("Game files imported ({0}).", what).Then(await PrepareAssetsAfterImportAsync(cancel));
        });
        return true;
    }

    /// <summary>
    /// The game assets right after an import (to-do 43): the import itself counts as done
    /// whatever happens here, so a failure or a cancel only adds a sentence (null: none).
    /// </summary>
    private async Task<Said?> PrepareAssetsAfterImportAsync(CancellationToken cancel)
    {
        try
        {
            await AssetCache.CarveAsync(folder, Progress(), cancel);
            return null;
        }
        catch (OperationCanceledException)
        {
            return S("Preparing the game assets was cancelled: the launcher does it the next time it starts.");
        }
        catch (IOException e)
        {
            return S("The game assets couldn't be prepared ({0}); the game plays without them.", Why(e));
        }
        finally
        {
            RefreshAssets();
        }
    }

    /// <summary>
    /// At launcher start (to-do 43): game assets that are missing or stale (an update from an
    /// older version brings no import; the game files changed) are made once, as a job of
    /// their own; Play waits for it like for an import.
    /// </summary>
    public async Task PrepareAssetsIfStaleAsync()
    {
        RefreshAssets();
        if (!GameFilesImported || !CanStartJob() || isGameRunning())
        {
            return;
        }

        if (await AssetCache.IsStaleAsync(folder, CancellationToken.None) != true || !CanStartJob())
        {
            return;
        }

        await RunJobAsync(FilesJob.Game, async cancel =>
        {
            await AssetCache.CarveAsync(folder, Progress(), cancel);
            return S("Game assets prepared from your game files.");
        });
        RefreshAssets();
    }

    private bool assetsPrepared;
    private int assetFiles;
    private long assetBytes;

    /// <summary>What the Game assets section says (to-do 43).</summary>
    public string AssetsText => !assetsPrepared
        ? T("Some features need parts of the game prepared from your game files. The launcher prepares them "
            + "after an import, or the next time it starts.")
        : T("Prepared from your game files ({0}, {1}). The launcher prepares them again after an import or when your game files change.",
            assetFiles == 1 ? T("1 file") : T("{0:N0} files", assetFiles), Size(assetBytes));

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskDeleteAssetsCommand))]
    public partial bool AssetsExist { get; set; }

    /// <summary>Delete Assets asked its question (Yes / No in its place).</summary>
    [ObservableProperty]
    public partial bool ConfirmDeleteAssets { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AssetsError))]
    public partial Said? AssetsErrorSaid { get; set; }

    public string? AssetsError => AssetsErrorSaid?.ToString();

    private void RefreshAssets()
    {
        (assetFiles, assetBytes) = AssetCache.Size();
        assetsPrepared = AssetCache.Prepared;
        AssetsExist = assetFiles > 0;
        OnPropertyChanged(nameof(AssetsText));
    }

    private static string Size(long bytes) =>
        bytes < 1024 * 1024 ? T("{0} KB", Math.Max(1, (bytes + 1023) / 1024)) : T("{0:0.#} MB", bytes / (1024.0 * 1024));

    [RelayCommand(CanExecute = nameof(CanDeleteAssets))]
    private void AskDeleteAssets()
    {
        AssetsErrorSaid = null;
        ConfirmDeleteAssets = true;
    }

    [RelayCommand]
    private void CancelDeleteAssets() => ConfirmDeleteAssets = false;

    /// <summary>Yes: the assets folder goes; the launcher prepares it again at its next start.</summary>
    [RelayCommand]
    private void DeleteAssets()
    {
        ConfirmDeleteAssets = false;
        if (!CanDeleteAssets())
        {
            return;     // the game or a job started meanwhile
        }

        try
        {
            AssetCache.Delete();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            AssetsErrorSaid = S("The game assets couldn't all be deleted: {0}", e.Message);
        }

        RefreshAssets();
    }

    private bool CanDeleteAssets() => AssetsExist && !IsBusy && !IsPrecompiling && !UserFiles.IsBusy && !isGameRunning();

    /// <summary>What the Texture packs section says (to-do 238): how packs work, and the last export.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ExportButtonText), nameof(TexturesText))]
    public partial int ExportedPngs { get; set; }

    private long exportedBytes;

    public string TexturesText => ExportedPngs == 0
        ? T("A texture pack swaps the game's textures for new art: PNGs in a folder of its own in textures\\packs, "
            + "named and laid out like the exported textures. Export Textures saves every texture of your game files "
            + "that a pack can change as a PNG (about 9,600 files and 600 MB, a minute or so) into textures\\dump, to "
            + "edit. The game doesn't read the dump folder itself.")
        : T("{0:N0} textures exported ({1}) into textures\\dump. To make a pack, put the ones you edit in a folder of its "
            + "own in textures\\packs, keeping their names and folders. Export Again adds any that are missing.",
            ExportedPngs, Size(exportedBytes));

    public string ExportButtonText => ExportedPngs > 0 ? T("Export Again") : T("Export Textures");

    private void RefreshTextures()
    {
        (int pngs, exportedBytes) = TextureExport.Size(folder);
        ExportedPngs = pngs;
        OnPropertyChanged(nameof(TexturesText));
    }

    /// <summary>
    /// Export Textures (to-do 238): <c>conker.exe --texture-export</c> into textures\dump; the
    /// PNGs already there are kept, so a stopped export goes on where it was.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanExportTextures))]
    private async Task ExportTexturesAsync()
    {
        await RunJobAsync(FilesJob.Textures, async cancel =>
        {
            TextureExport.Result r;
            try
            {
                r = await TextureExport.ExportAsync(folder, Progress(), cancel);
            }
            catch (OperationCanceledException)
            {
                return S("Stopped. The textures saved so far are kept; Export Again goes on from there.");
            }

            Said text = r.Written == 0 && r.Failed == 0 ? S("All {0:N0} textures were exported already.", r.Total)
                : r.Kept > 0 ? S("Exported {0:N0} textures ({1:0} MB); {2:N0} were there already.", r.Written, r.MegaBytes, r.Kept)
                : S("Exported {0:N0} textures ({1:0} MB).", r.Written, r.MegaBytes);
            return text.Then(r.Failed > 0 ? S("{0:N0} couldn't be saved.", r.Failed) : null);
        });
    }

    private bool CanExportTextures() => GameFilesImported && CanStartJob();

    [RelayCommand]
    private void OpenExportedTextures() => OpenFolder(TextureExport.Folder(folder));

    /// <summary>Packs Folder (to-do 288 part 2d; was Textures Folder): textures\packs, made when missing.</summary>
    [RelayCommand]
    private void OpenPacksFolder() => OpenFolder(TexturePacks.Folder(folder));

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task AddPatchAsync()
    {
        string? source = await picker.PickFileAsync(T("Choose the decensor patch (.7z or .zip)"), T("Decensor patch (.7z, .zip)"),
            [.. DecensorPatch.Extensions.Select(x => "*" + x)]);
        if (source is null)
        {
            return;
        }

        await RunJobAsync(FilesJob.Decensor, cancel => Task.Run(() =>
        {
            decensor.Install(source, null, Progress(), cancel);
            return S("Decensor patch added. Switch it on in the game's F2 menu (Uncensored).");
        }, cancel));
    }

    [RelayCommand(CanExecute = nameof(CanStartJob))]
    private async Task DownloadPatchAsync()
    {
        await RunJobAsync(FilesJob.Decensor, async cancel =>
        {
            ProgressSaid = S("Looking for the latest release");
            ReleaseInfo latest = await GitHubReleases.GetLatestAsync(DecensorPatch.Repo, DecensorPatch.Extensions, cancel);
            LatestDecensor = latest;
            await decensor.DownloadAndInstallAsync(latest, Progress(), cancel);
            return S("Decensor patch {0} installed. Switch it on in the game's F2 menu (Uncensored).", latest.Tag);
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
        PrecompileErrorSaid = null;
        if (!File.Exists(folder.GameExe))
        {
            PrecompileErrorSaid = S("conker.exe is missing from the launcher's folder.");
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
            PrecompileErrorSaid = S("The game didn't start: {0}", e.Message);
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
        PrecompileErrorSaid = null;
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
            PrecompileErrorSaid = S("Some shaders couldn't be deleted: {0}", e.Message);
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
        ProgressSaid = p.Text;
        ProgressFraction = p.Fraction;
        OnPropertyChanged(nameof(ProgressPercent));
    });

    private async Task RunJobAsync(FilesJob kind, Func<CancellationToken, Task<Said>> work)
    {
        using CancellationTokenSource cancel = new();
        job = cancel;
        JobKind = kind;
        IsBusy = true;
        StatusSaid = null;
        ProgressSaid = S("Starting");
        ProgressFraction = null;
        StateChanged?.Invoke(this, EventArgs.Empty);
        try
        {
            StatusSaid = await work(cancel.Token);
            StatusIsError = false;
        }
        catch (OperationCanceledException)
        {
            StatusSaid = S("Cancelled. Nothing was changed.");
            StatusIsError = false;
        }
        catch (ImportException e)
        {
            StatusSaid = e.Text;
            StatusIsError = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or HttpRequestException
                                       or System.Text.Json.JsonException or KeyNotFoundException or InvalidOperationException)
        {
            StatusSaid = e is HttpRequestException ? S("The download failed: {0}", Why(e)) : S("Something went wrong: {0}", Why(e));
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
