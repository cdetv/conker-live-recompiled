namespace ConkerLauncher.Services;

/// <summary>What only the window can do: an Open dialog for one local file, the clipboard.</summary>
public interface IFilePicker
{
    /// <summary>The chosen file's path, or null when the player cancelled.</summary>
    Task<string?> PickFileAsync(string title, string filterName, IReadOnlyList<string> patterns);

    /// <summary>Puts the text on the clipboard.</summary>
    Task CopyTextAsync(string text);
}
