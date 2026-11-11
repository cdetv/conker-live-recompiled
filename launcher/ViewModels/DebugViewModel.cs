using System.Diagnostics;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>A chapter's part for the save editor: its console word (<c>slop2</c>) and names.</summary>
public sealed record PartChoice(string Word, int Number, string Name)
{
    public string Label => $"{Number}  {Name}";
}

public sealed record ChapterChoice(string Name, IReadOnlyList<PartChoice> Parts);

public sealed record GameChoice(int Slot, bool HasSave)
{
    public string Label => HasSave ? $"Game {Slot}" : $"Game {Slot} (no save yet)";
}

/// <summary>
/// The Debug tab (to-do 144, SPEC 9.3): the Debug Mode checkbox (launcher.ini <c>debug_mode</c>;
/// Play then starts <c>conker.exe --debug</c>) and the save editor (to-do 147): the game's own
/// <c>conker.exe --edit-save &lt;slot&gt; &lt;word&gt;&lt;number&gt;</c>, run hidden, as Report Bug runs
/// <c>--bug-report</c>. Not while the game runs: its next save would undo the edit.
/// </summary>
public sealed partial class DebugViewModel : ObservableObject
{
    // The user's list (SPEC 10.3), the game's counts; the words are conker.exe's.
    private static readonly (string Word, string Chapter, string[] Parts)[] Table =
    [
        ("hung", "Hungover", ["Scaredy Birdy", "Panhandled", "Gargoyle"]),
        ("windy", "Windy", ["Mrs. Bee", "Poo Cabin", "Pruned", "Yeehaa!", "Sewage Sucks", "Great Balls of Poo",
                            "Wasp's Revenge", "Mr. Barrel"]),
        ("bat", "Bat's Tower", ["Mrs. Catfish", "Barry's Mates", "Cog's Revenge", "The Combination", "Blast Doors",
                                "Clang's Lair", "Pisstastic", "Brass Monkeys", "Bullfish's Revenge"]),
        ("barn", "Barn Boys", ["Marvin", "Mad Pitchfork", "Sunny Days", "Barry + Co.", "Buff You", "Haybot Wars",
                               "Frying Tonight", "Slam Dunk"]),
        ("slop", "Sloprano", ["Corn off the Cob", "Sweet Melody", "U-Bend Blues", "The Bluff"]),
        ("uga", "Uga Buga", ["Drunken Nights", "Sacrifice", "Phlegm", "Worship", "Rock Solid", "Bomb Run", "Mugged",
                             "Raptor Food", "Buga the Knut"]),
        ("spooky", "Spooky", ["Mr. Death", "Count Batula", "Zombies", "Mr. Barrel"]),
        ("war", "It's War", ["It's War", "Power's Off", "TNT", "The Assault", "Sole Survivor", "Casualty Dept.",
                             "Saving Private Rodent", "Chemical Warfare", "The Tower", "Little Girl", "The Experiment",
                             "Countdown", "Peace at Last"]),
        ("heist", "Heist", ["The Windmill's Dead", "Enter The Vertex", "The Vault", "End Cutscene"]),
    ];

    private readonly GameFolder folder;
    private readonly Func<bool> isGameRunning;
    private bool loading;

    public DebugViewModel(GameFolder folder, Func<bool> isGameRunning)
    {
        this.folder = folder;
        this.isGameRunning = isGameRunning;
        Chapters = Table.Select(t => new ChapterChoice(t.Chapter,
            t.Parts.Select((p, i) => new PartChoice(t.Word + (i + 1), i + 1, p)).ToList())).ToList();
        SelectedChapter = Chapters[0];
        Load();
    }

    /// <summary>Play starts the game with <c>--debug</c>; remembered across launches.</summary>
    [ObservableProperty]
    public partial bool DebugMode { get; set; }

    partial void OnDebugModeChanged(bool value)
    {
        if (!loading)
        {
            LauncherPrefs.DebugMode = value;
        }
    }

    /// <summary>Reads launcher.ini and the save slots (again once the user files are in place).</summary>
    public void Load()
    {
        loading = true;
        DebugMode = LauncherPrefs.DebugMode;
        loading = false;
        int slot = SelectedGame?.Slot ?? 1;
        Games = [.. Enumerable.Range(1, 3).Select(n => new GameChoice(n,
            File.Exists(Path.Combine(SaveFolder.Root, "saves", $"Game{n}", "conker.bin"))))];
        SelectedGame = Games.FirstOrDefault(g => g.Slot == slot);
    }

    [ObservableProperty]
    public partial IReadOnlyList<GameChoice> Games { get; set; } = [];

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskEditCommand))]
    public partial GameChoice? SelectedGame { get; set; }

    public IReadOnlyList<ChapterChoice> Chapters { get; }

    [ObservableProperty]
    public partial ChapterChoice? SelectedChapter { get; set; }

    partial void OnSelectedChapterChanged(ChapterChoice? value) => SelectedPart = value?.Parts[0];

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskEditCommand))]
    public partial PartChoice? SelectedPart { get; set; }

    [ObservableProperty]
    public partial bool IsAsking { get; set; }

    [ObservableProperty]
    public partial string Question { get; set; } = "";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskEditCommand))]
    public partial bool IsBusy { get; set; }

    [ObservableProperty]
    public partial string? StatusText { get; set; }

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    private bool CanEdit() => SelectedGame is { HasSave: true } && SelectedPart is not null && !IsBusy;

    [RelayCommand(CanExecute = nameof(CanEdit))]
    private void AskEdit()
    {
        if (isGameRunning())
        {
            StatusText = "The game is running: close it first (its next save would put the old place back).";
            StatusIsError = true;
            return;
        }

        Question = $"Edit Game {SelectedGame!.Slot} to {SelectedChapter!.Name} {SelectedPart!.Number}, {SelectedPart.Name}?";
        StatusText = null;
        IsAsking = true;
    }

    [RelayCommand]
    private void CancelEdit() => IsAsking = false;

    [RelayCommand]
    private async Task ConfirmEditAsync()
    {
        IsAsking = false;
        if (SelectedGame is not { } game || SelectedPart is not { } part)
        {
            return;
        }

        IsBusy = true;
        try
        {
            StatusText = await RunAsync(game.Slot, part.Word);
            StatusIsError = false;
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception or OperationCanceledException)
        {
            StatusText = e is OperationCanceledException ? "conker.exe didn't answer." : e.Message;
            StatusIsError = true;
        }
        finally
        {
            IsBusy = false;
            Load();
        }
    }

    private async Task<string> RunAsync(int slot, string word)
    {
        if (!File.Exists(folder.GameExe))
        {
            throw new IOException("conker.exe is missing from the launcher's folder.");
        }

        var start = new ProcessStartInfo(folder.GameExe)
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
        };
        start.ArgumentList.Add("--edit-save");
        start.ArgumentList.Add(slot.ToString(System.Globalization.CultureInfo.InvariantCulture));
        start.ArgumentList.Add(word);
        start.ArgumentList.Add("--save-dir");
        start.ArgumentList.Add(SaveFolder.Root);
        start.ArgumentList.Add("--game-dir");
        start.ArgumentList.Add(folder.GameDir);

        using Process process = OwnProcesses.Start(start) ?? throw new IOException("conker.exe didn't start.");
        string output, errors;
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            Task<string> o = process.StandardOutput.ReadToEndAsync(timeout.Token);
            Task<string> e = process.StandardError.ReadToEndAsync(timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
            output = await o;
            errors = await e;
        }
        finally
        {
            OwnProcesses.Forget(process);
        }

        if (process.ExitCode != 0)
        {
            string why = errors.Split('\n').FirstOrDefault(l => l.StartsWith("Edit Save: ", StringComparison.Ordinal))
                ?.Replace("Edit Save: ", "", StringComparison.Ordinal).Trim() ?? $"conker.exe ended with {process.ExitCode}.";
            throw new IOException(why);
        }

        return output.Trim();
    }
}
