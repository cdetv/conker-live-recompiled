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

    public static string AppDataText => $"AppData ({UserFiles.AppDataPath})";

    public string? Note => SaveFolder.Override is { } dir ? $"Development override: {dir}"
        : !files.GameFolderWritable ? "The game folder can't be written to, so they are kept in AppData."
        : null;

    [ObservableProperty]
    public partial string? ProgressText { get; set; }

    /// <summary>The last move's result.</summary>
    [ObservableProperty]
    public partial string? StatusText { get; set; }

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    /// <summary>The save slot the pop-up asks about.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Question), nameof(SaveLine), nameof(DeleteText), nameof(DeleteQuestion), nameof(TransferTip), nameof(BackupTip))]
    public partial SaveConflict? Conflict { get; set; }

    [ObservableProperty]
    public partial bool ConfirmDelete { get; set; }

    public string Question => Conflict?.From == UserFilesPlace.GameFolder
        ? "Save Data from the game folder is also located in AppData, what would you like to do?"
        : "Save Data from older version located in AppData, what would you like to do?";

    public string SaveLine => Conflict is not { } c ? ""
        : $"\"{c.Name}\"\n{Label(c.From)}: last saved {When(c.SourceTime)}\n" +
          $"{Label(Other(c.From))}: last saved {When(c.TargetTime)}";

    public string DeleteText => Conflict?.From == UserFilesPlace.GameFolder ? "Delete Game Folder Save" : "Delete AppData Save";

    public string DeleteQuestion => Conflict is not { } c ? ""
        : $"Delete the {UserFiles.NameOf(c.From)} save \"{c.Name}\" for good? It can't be brought back.";

    public string TransferTip => Conflict is not { } c ? ""
        : $"Copies {UserFiles.NameOf(Other(c.From))}'s save to the backups folder, then moves the {UserFiles.NameOf(c.From)} save in its place";

    public string BackupTip => Conflict is not { } c ? ""
        : $"Moves the {UserFiles.NameOf(c.From)} save to the backups folder; {UserFiles.NameOf(Other(c.From))}'s save stays";

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
            StatusText = $"The save folders couldn't be renamed: {e.Message}. The game renames them at its start.";
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
            StatusText = "Close the game first: it is using the user files.";
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
            StatusText = $"The choice couldn't be saved: {e.Message}";
            StatusIsError = true;
            Sync();
            return;
        }

        await RunMoveAsync();
    }

    private async Task RunMoveAsync()
    {
        IsBusy = true;
        StatusText = null;
        ProgressText = "Looking at the user files";
        StateChanged?.Invoke(this, EventArgs.Empty);
        UserFilesPlace from = Other(files.Place);
        int failed = 0, moved = 0;
        try
        {
            var progress = new Progress<JobProgress>(p => ProgressText = p.Text);
            MoveResult result = await Task.Run(() => files.MoveIn(progress, CancellationToken.None));
            moved = result.Moved;
            failed = result.Failed;
            ProgressText = null;
            foreach (SaveConflict conflict in result.Conflicts)
            {
                ConflictChoice choice = await AskAsync(conflict);
                await Task.Run(() => UserFiles.Resolve(conflict, choice));
            }

            StatusText = failed > 0
                ? $"{failed} file(s) couldn't be moved from {UserFiles.NameOf(from)} (in use?). The launcher tries again at its next start."
                : moved > 0 || result.Conflicts.Count > 0 ? $"User files moved from {UserFiles.NameOf(from)} to {UserFiles.NameOf(files.Place)}." : null;
            StatusIsError = failed > 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            StatusText = $"Moving the user files stopped: {e.Message}. What is left is moved at the next start.";
            StatusIsError = true;
        }
        finally
        {
            Conflict = null;
            ConfirmDelete = false;
            ProgressText = null;
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

    private static string Label(UserFilesPlace place) => place == UserFilesPlace.GameFolder ? "Game folder" : "AppData";

    private static string When(DateTime t) => t.ToString("d MMM yyyy, HH:mm", CultureInfo.CurrentCulture);
}
