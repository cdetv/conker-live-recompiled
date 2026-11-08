using System.Diagnostics;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>
/// The Files tab's Report Bug pop-up (to-do 12d, SPEC 9.2 "Launcher"): for a crash noticed later.
/// The zip is the game's own: <c>conker.exe --bug-report --desc-file</c>, run hidden, prints its
/// path, the prefilled GitHub page and the issue's text.
/// </summary>
public sealed partial class ReportBugViewModel(GameFolder folder, IFilePicker window) : ObservableObject
{
    private string? zipPath, issueUrl, issueText;

    public enum ReportStage
    {
        Edit,
        Working,
        Done,
        Failed,
    }

    [ObservableProperty]
    public partial bool IsOpen { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsEdit), nameof(IsWorking), nameof(IsDone), nameof(IsFailed))]
    public partial ReportStage Stage { get; set; }

    public bool IsEdit => Stage == ReportStage.Edit;

    public bool IsWorking => Stage == ReportStage.Working;

    public bool IsDone => Stage == ReportStage.Done;

    public bool IsFailed => Stage == ReportStage.Failed;

    /// <summary>Kept after Cancel; emptied once a report was made (as in the game's window).</summary>
    [ObservableProperty]
    public partial string Description { get; set; } = "";

    [ObservableProperty]
    public partial bool IncludeSaves { get; set; } = true;

    /// <summary>Done: "Report saved: name". Failed: why.</summary>
    [ObservableProperty]
    public partial string? ResultText { get; set; }

    /// <summary>What the last button did.</summary>
    [ObservableProperty]
    public partial string? Note { get; set; }

    [RelayCommand]
    private void Open()
    {
        if (Stage != ReportStage.Working)
        {
            if (Stage == ReportStage.Done)
            {
                Description = "";
            }

            Stage = ReportStage.Edit;
        }

        IncludeSaves = true;
        Note = null;
        IsOpen = true;
    }

    [RelayCommand]
    private void Close() => IsOpen = false;

    [RelayCommand]
    private void Back() => Stage = ReportStage.Edit;

    [RelayCommand]
    private async Task CreateAsync()
    {
        Stage = ReportStage.Working;
        string descFile = Path.Combine(Path.GetTempPath(), $"conker_report_{Environment.ProcessId}.txt");
        try
        {
            await File.WriteAllTextAsync(descFile, Description, new UTF8Encoding(false));
            (zipPath, issueUrl, issueText) = await RunGameAsync(descFile);
            ResultText = $"Report saved: {Path.GetFileName(zipPath)}";
            Stage = ReportStage.Done;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException
                                       or System.ComponentModel.Win32Exception)
        {
            ResultText = $"The report could not be made: {e.Message}";
            Stage = ReportStage.Failed;
        }
        finally
        {
            try
            {
                File.Delete(descFile);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // In the temp folder; nothing more to do.
            }
        }
    }

    /// <summary>The zip's path, the issue page and its text: conker.exe's three-part stdout.</summary>
    private async Task<(string Zip, string? Url, string? Text)> RunGameAsync(string descFile)
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
        start.ArgumentList.Add("--bug-report");
        start.ArgumentList.Add("--save-dir");
        start.ArgumentList.Add(SaveFolder.Root);
        start.ArgumentList.Add("--desc-file");
        start.ArgumentList.Add(descFile);
        if (!IncludeSaves)
        {
            start.ArgumentList.Add("--no-saves");
        }

        using Process process = OwnProcesses.Start(start) ?? throw new IOException("conker.exe didn't start.");
        string output, errors;
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
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
            string why = errors.Split('\n').FirstOrDefault(l => l.StartsWith("Report Bug: ", StringComparison.Ordinal))
                ?.Replace("Report Bug: ", "", StringComparison.Ordinal).Trim() ?? $"conker.exe ended with {process.ExitCode}.";
            throw new IOException(why);
        }

        string[] parts = output.Split('\n', 3);
        string zip = parts[0].TrimEnd('\r');
        if (zip.Length == 0)
        {
            throw new IOException("conker.exe gave no report.");
        }

        return (zip, parts.Length > 1 ? parts[1].TrimEnd('\r') : null, parts.Length > 2 ? parts[2] : null);
    }

    /// <summary>Explorer with the zip selected.</summary>
    [RelayCommand]
    private void OpenFolder()
    {
        if (zipPath is null)
        {
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{zipPath}\"") { UseShellExecute = false })?.Dispose();
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            Note = $"The folder could not be opened: {e.Message}";
        }
    }

    /// <summary>The text on the clipboard first (for a page that comes up empty), then the page.</summary>
    [RelayCommand]
    private async Task OpenIssueAsync()
    {
        if (issueUrl is not { Length: > 0 })
        {
            Note = "The issue page could not be opened.";
            return;
        }

        if (issueText is not null)
        {
            await window.CopyTextAsync(issueText);
        }

        try
        {
            Process.Start(new ProcessStartInfo(issueUrl) { UseShellExecute = true })?.Dispose();
            Note = "Opening the issue page in your browser; its text is on the clipboard too.";
        }
        catch (System.ComponentModel.Win32Exception)
        {
            Note = "The issue page could not be opened.";
        }
    }
}
