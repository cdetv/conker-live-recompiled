using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// The Files tab's Steam section (to-do 285, SPEC 9.3): Add to Steam, on real Windows only
/// (Linux and the Steam Deck run <c>add-to-steam.sh</c> from the release zip).
/// </summary>
public sealed partial class SteamViewModel : ObservableObject
{
    private readonly SteamInstall? steam;

    public SteamViewModel()
    {
        Available = SteamShortcut.Supported;
        steam = Available ? SteamShortcut.Find() : null;
    }

    /// <summary>The section shows (real Windows; hidden under Wine).</summary>
    public bool Available { get; }

    public string Text => steam is null
        ? "Steam wasn't found on this PC."
        : "Adds the launcher to your Steam library as a non-Steam game, with artwork from SteamGridDB. Clicking it again updates it.";

    /// <summary>Steam is open: Yes / No in the button's place first (it closes meanwhile).</summary>
    [ObservableProperty]
    public partial bool IsAsking { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(AskAddCommand), nameof(AddCommand))]
    public partial bool IsBusy { get; set; }

    /// <summary>What Add to Steam is doing, did, or why it stopped.</summary>
    [ObservableProperty]
    public partial string? Status { get; set; }

    [ObservableProperty]
    public partial bool StatusIsError { get; set; }

    [RelayCommand(CanExecute = nameof(CanAdd))]
    private async Task AskAddAsync()
    {
        if (SteamShortcut.IsRunning())
        {
            Status = null;
            IsAsking = true;
            return;
        }

        await AddAsync();
    }

    [RelayCommand]
    private void CancelAdd() => IsAsking = false;

    [RelayCommand(CanExecute = nameof(CanAdd))]
    private async Task AddAsync()
    {
        IsAsking = false;
        if (steam is null || Environment.ProcessPath is not { } launcher)
        {
            return;
        }

        IsBusy = true;
        StatusIsError = false;
        // Progress lines arrive later than the result can: none after it.
        bool done = false;
        try
        {
            SteamResult r = await SteamShortcut.AddAsync(steam, launcher, new Progress<string>(text =>
            {
                if (!done)
                {
                    Status = text;
                }
            }));
            done = true;
            string pictures = r.Pictures == r.PicturesWanted
                ? "with its artwork"
                : r.Pictures == 0 ? "without artwork (SteamGridDB couldn't be reached)"
                : $"with {r.Pictures} of {r.PicturesWanted} pictures (SteamGridDB couldn't send the rest)";
            Status = $"{(r.Updated ? "Updated in" : "Added to")} Steam for {r.Account}, {pictures}. Steam is starting.";
        }
        catch (SteamException e)
        {
            done = true;
            Status = e.Message;
            StatusIsError = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            done = true;
            Status = $"Adding it to Steam failed: {e.Message}";
            StatusIsError = true;
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool CanAdd() => steam is not null && !IsBusy;
}
