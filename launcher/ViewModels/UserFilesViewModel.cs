using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// Where the user files are kept (Files tab) and moving them (SPEC 9.3): at start-up from the
/// place not in use, and when the player switches places. A save slot in both places asks the
/// player, one slot at a time.
/// </summary>
public sealed partial class UserFilesViewModel : ObservableObject
{
    private readonly UserFiles files;
    private readonly Func<bool> gameRunning;
    private TaskCompletionSource<ConflictChoice>? answer;
    private bool syncing;

    public UserFilesViewModel(GameFolder folder, Func<bool> gameRunning)
    {
        files = new UserFiles(folder);
        this.gameRunning = gameRunning;
        if (SaveFolder.Override is null)
        {
            files.CheckWritable();
            SaveFolder.Files = files;
        }

        Sync();
        // A language switch (to-do 289): every text here said again.
        Lang.Changed += (_, _) => OnPropertyChanged(string.Empty);
    }

    /// <summary>Raised when a move starts or ends (Play, Settings and Controls wait for it).</summary>
    public event EventHandler? StateChanged;

    /// <summary>A move is running or a save slot waits for the player's choice.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanChoose))]
    public partial bool IsBusy { get; set; }

    /// <summary>The Files tab's choice; changing it moves the files.</summary>
    [ObservableProperty]
    public partial bool InGameFolder { get; set; }

    public bool InAppData
    {
        get => !InGameFolder;
        set => InGameFolder = !value;
    }

    /// <summary>Settable from the launcher: the game folder can be written and no development override is on.</summary>
    public bool CanChoose => SaveFolder.Override is null && files.GameFolderWritable && !IsBusy;

    // A radio button's text takes "_" as its access key mark (it vanished: "_work" showed as "work", and so would a
    // user name like "john_doe"); "__" shows one.
    public static string AppDataText => $"AppData ({UserFiles.AppDataPath.Replace("_", "__", StringComparison.Ordinal)})";

    // The development override stays English (CONKER_LAUNCHER_SAVE_DIR; no player sees it).
    public string? Note => SaveFolder.Override is { } dir ? $"Development override: {dir}"
        : !files.GameFolderWritable ? T("The game folder can't be written to, so they are kept in AppData.")
        : null;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ProgressText))]
    public partial Said? ProgressSaid { get; set; }

    public string? ProgressText => ProgressSaid?.ToString();

    /// <summary>The last move's result.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusText))]
    public partial Said? StatusSaid { get; set; }

    public string? StatusText => StatusSaid?.ToString();

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    /// <summary>The save slot the pop-up asks about.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Question), nameof(SaveLine), nameof(DeleteText), nameof(DeleteQuestion), nameof(TransferTip), nameof(BackupTip))]
    public partial SaveConflict? Conflict { get; set; }

    [ObservableProperty]
    public partial bool ConfirmDelete { get; set; }

    public string Question => Conflict?.From == UserFilesPlace.GameFolder
        ? T("Save Data from the game folder is also located in AppData, what would you like to do?")
        : T("Save Data from older version located in AppData, what would you like to do?");

    public string SaveLine => Conflict is not { } c ? ""
        : $"\"{c.Name}\"\n{T("{0}: last saved {1}", Label(c.From), Lang.Date(c.SourceTime))}\n"
          + T("{0}: last saved {1}", Label(Other(c.From)), Lang.Date(c.TargetTime));

    public string DeleteText => Conflict?.From == UserFilesPlace.GameFolder ? T("Delete Game Folder Save") : T("Delete AppData Save");

    // Whole sentences for each place (a place's name can't be dropped into another language's sentence).
    public string DeleteQuestion => Conflict is not { } c ? ""
        : c.From == UserFilesPlace.GameFolder
            ? T("Delete the game folder save \"{0}\" for good? It can't be brought back.", c.Name)
            : T("Delete the AppData save \"{0}\" for good? It can't be brought back.", c.Name);

    public string TransferTip => Conflict is not { } c ? ""
        : c.From == UserFilesPlace.GameFolder
            ? T("Copies AppData's save to the backups folder, then moves the game folder save in its place")
            : T("Copies the game folder's save to the backups folder, then moves the AppData save in its place");

    public string BackupTip => Conflict is not { } c ? ""
        : c.From == UserFilesPlace.GameFolder
            ? T("Moves the game folder save to the backups folder; AppData's save stays")
            : T("Moves the AppData save to the backups folder; the game folder's save stays");

    /// <summary>
    /// Names the save folders (an old layout is converted) and moves in what the place not in
    /// use holds (start-up, and after the game closes).
    /// </summary>
    public async Task MoveInAsync()
    {
        if (SaveFolder.Override is not null || IsBusy || gameRunning())
        {
            return;
        }

        if (files.OtherPlaceHasFiles)
        {
            await RunMoveAsync();
            return;
        }

        try
        {
            await Task.Run(files.ConvertSaveNames);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            StatusSaid = S("The save folders couldn't be renamed: {0}. The game renames them at its start.", e.Message);
            StatusIsError = true;
        }
    }

    partial void OnInGameFolderChanged(bool value)
    {
        OnPropertyChanged(nameof(InAppData));
        if (!syncing)
        {
            _ = SwitchAsync(value ? UserFilesPlace.GameFolder : UserFilesPlace.AppData);
        }
    }

    private async Task SwitchAsync(UserFilesPlace place)
    {
        if (gameRunning())
        {
            StatusSaid = S("Close the game first: it is using the user files.");
            StatusIsError = true;
            Sync();
            return;
        }

        try
        {
            files.Choose(place);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            StatusSaid = S("The choice couldn't be saved: {0}", e.Message);
            StatusIsError = true;
            Sync();
            return;
        }

        await RunMoveAsync();
    }

    private async Task RunMoveAsync()
    {
        IsBusy = true;
        StatusSaid = null;
        ProgressSaid = S("Looking at the user files");
        StateChanged?.Invoke(this, EventArgs.Empty);
        UserFilesPlace from = Other(files.Place);
        int failed = 0, moved = 0;
        try
        {
            var progress = new Progress<JobProgress>(p => ProgressSaid = p.Text);
            MoveResult result = await Task.Run(() => files.MoveIn(progress, CancellationToken.None));
            moved = result.Moved;
            failed = result.Failed;
            ProgressSaid = null;
            foreach (SaveConflict conflict in result.Conflicts)
            {
                ConflictChoice choice = await AskAsync(conflict);
                await Task.Run(() => UserFiles.Resolve(conflict, choice));
            }

            bool fromGame = from == UserFilesPlace.GameFolder;
            StatusSaid = failed > 0
                ? fromGame
                    ? S("{0} file(s) couldn't be moved from the game folder (in use?). The launcher tries again at its next start.", failed)
                    : S("{0} file(s) couldn't be moved from AppData (in use?). The launcher tries again at its next start.", failed)
                : moved > 0 || result.Conflicts.Count > 0
                    ? fromGame ? S("User files moved from the game folder to AppData.") : S("User files moved from AppData to the game folder.")
                : null;
            StatusIsError = failed > 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            StatusSaid = S("Moving the user files stopped: {0}. What is left is moved at the next start.", e.Message);
            StatusIsError = true;
        }
        finally
        {
            Conflict = null;
            ConfirmDelete = false;
            ProgressSaid = null;
            IsBusy = false;
            Sync();
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    private Task<ConflictChoice> AskAsync(SaveConflict conflict)
    {
        answer = new TaskCompletionSource<ConflictChoice>();
        ConfirmDelete = false;
        Conflict = conflict;
        return answer.Task;
    }

    [RelayCommand]
    private void BackupAndTransfer() => Answer(ConflictChoice.BackupAndTransfer);

    [RelayCommand]
    private void BackupAndDelete() => Answer(ConflictChoice.BackupAndDelete);

    [RelayCommand]
    private void AskDelete() => ConfirmDelete = true;

    [RelayCommand]
    private void CancelDelete() => ConfirmDelete = false;

    [RelayCommand]
    private void Delete() => Answer(ConflictChoice.DeleteSource);

    private void Answer(ConflictChoice choice)
    {
        Conflict = null;
        ConfirmDelete = false;
        answer?.TrySetResult(choice);
    }

    private void Sync()
    {
        syncing = true;
        InGameFolder = SaveFolder.Override is null && files.Place == UserFilesPlace.GameFolder;
        syncing = false;
        OnPropertyChanged(nameof(CanChoose));
        OnPropertyChanged(nameof(Note));
    }

    private static UserFilesPlace Other(UserFilesPlace place) =>
        place == UserFilesPlace.GameFolder ? UserFilesPlace.AppData : UserFilesPlace.GameFolder;

    private static string Label(UserFilesPlace place) => place == UserFilesPlace.GameFolder ? T("Game folder") : "AppData";
}
