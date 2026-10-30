namespace ConkerLauncher.Services;

/// <summary>An Open dialog for one local file (the window provides it).</summary>
public interface IFilePicker
{
    /// <summary>The chosen file's path, or null when the player cancelled.</summary>
    Task<string?> PickFileAsync(string title, string filterName, IReadOnlyList<string> patterns);
}
