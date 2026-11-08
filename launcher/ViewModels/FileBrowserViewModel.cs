using System.Globalization;
using System.Security;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>A row of the file browser's list: a folder or a file.</summary>
public sealed record BrowserEntry(string Name, string FullPath, bool IsFolder, long Size, DateTime Modified, bool Matches)
{
    public string SizeText => IsFolder ? "" : FileBrowserViewModel.SizeText(Size);

    public string DateText => Modified.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);

    /// <summary>A file this choice doesn't take (listed with "Show all files"): drawn dimmed.</summary>
    public bool IsOther => !IsFolder && !Matches;

    public override string ToString() => Name;
}

public enum PlaceKind
{
    Launcher,
    Recent,
    Folder,
    Drive,
}

/// <summary>A Places row: the launcher's folder, a recent folder, Desktop / Downloads / Documents, a drive.</summary>
public sealed record BrowserPlace(string Label, string Path, string? Detail, PlaceKind Kind)
{
    public override string ToString() => Label;
}

/// <summary>Where an address typed or pasted into the browser leads: a folder, a file, or why neither.</summary>
public sealed record AddressTarget(string? Folder, string? File, string? Error);

/// <summary>
/// The launcher's own file browser (to-do 122, SPEC 9.3): one per file choice (import the
/// game, add the decensor patch), opened by <c>MainWindow.PickFileAsync</c> in place of the
/// Windows Open dialog. Opens in the launcher's own folder; Places down the side; folders
/// first, then the files this choice takes (X / "Show all files": every file); an address
/// field; files dropped from Explorer. <see cref="Result"/> is the chosen file, or null.
/// </summary>
/// <remarks>
/// Folders are read on a background task; a newer open wins over a slow older one (network
/// drives). Hidden and system entries are skipped. A game folder is picked by its
/// <c>default.xbe</c> (no other <c>.xbe</c> counts). The folder a file is picked from goes
/// to <see cref="LauncherPrefs.RecentFolders"/>.
/// </remarks>
public sealed partial class FileBrowserViewModel : ObservableObject
{
    private const string GameXbe = "default.xbe";

    private readonly TaskCompletionSource<string?> result = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Func<Task<string?>>? windowsDialog;
    private List<BrowserEntry> all = [];
    private int loadId;

    /// <param name="title">What to choose, shown at the top.</param>
    /// <param name="filterName">The types taken, in words (the error lines use it).</param>
    /// <param name="extensions">The types taken: <c>.iso</c> or <c>*.iso</c>.</param>
    /// <param name="startFolder">Where the browser opens: the launcher's own folder.</param>
    /// <param name="windowsDialog">"Windows dialog...": the old Open dialog, its file or null.</param>
    public FileBrowserViewModel(string title, string filterName, IReadOnlyList<string> extensions, string startFolder,
        Func<Task<string?>>? windowsDialog = null)
    {
        Title = title;
        FilterName = filterName;
        Extensions = [.. extensions.Select(e => e.TrimStart('*').ToLowerInvariant())];
        StartFolder = startFolder;
        this.windowsDialog = windowsDialog;
    }

    public string Title { get; }

    public string FilterName { get; }

    public IReadOnlyList<string> Extensions { get; }

    public string StartFolder { get; }

    /// <summary>The chosen file's full path, or null when cancelled.</summary>
    public Task<string?> Result => result.Task;

    public bool CanUseWindowsDialog => windowsDialog is not null;

    /// <summary>The folder listed, or null before the first one opened.</summary>
    [ObservableProperty]
    public partial string? CurrentFolder { get; set; }

    /// <summary>The address field: the current folder, or what the player typed or pasted.</summary>
    [ObservableProperty]
    public partial string AddressText { get; set; } = "";

    [ObservableProperty]
    public partial IReadOnlyList<BrowserEntry> Entries { get; set; } = [];

    [ObservableProperty]
    public partial BrowserEntry? SelectedEntry { get; set; }

    [ObservableProperty]
    public partial IReadOnlyList<BrowserPlace> Places { get; set; } = [];

    /// <summary>The Places row for the folder listed, if it is one (shown lit).</summary>
    [ObservableProperty]
    public partial BrowserPlace? CurrentPlace { get; private set; }

    /// <summary>Every file, not only the ones this choice takes (X on the pad).</summary>
    [ObservableProperty]
    public partial bool ShowAllFiles { get; set; }

    /// <summary>A folder is being read (shown as "Reading..." once it takes a moment).</summary>
    [ObservableProperty]
    public partial bool IsReading { get; set; }

    /// <summary>The line under the list: why something didn't work, or null.</summary>
    [ObservableProperty]
    public partial string? Message { get; set; }

    /// <summary>What an empty list says: an empty folder, or no file of the types taken.</summary>
    [ObservableProperty]
    public partial string? EmptyText { get; set; }

    partial void OnShowAllFilesChanged(bool value) => ApplyFilter(SelectedEntry?.Name);

    partial void OnCurrentFolderChanged(string? value) => UpdateCurrentPlace();

    partial void OnPlacesChanged(IReadOnlyList<BrowserPlace> value) => UpdateCurrentPlace();

    private void UpdateCurrentPlace() =>
        CurrentPlace = CurrentFolder is { } folder ? Places.FirstOrDefault(p => SamePath(p.Path, folder)) : null;

    /// <summary>Opens the start folder and fills Places (on a background task: a sleeping network drive can take seconds).</summary>
    public async Task StartAsync()
    {
        Task<List<BrowserPlace>> places = Task.Run(() => BuildPlaces(StartFolder, LauncherPrefs.RecentFolders));
        await OpenFolderAsync(StartFolder);
        Places = await places;
    }

    /// <summary>Lists a folder; <paramref name="select"/> = the entry to select (the folder just left). False: not opened (Message says why).</summary>
    public async Task<bool> OpenFolderAsync(string path, string? select = null)
    {
        int id = ++loadId;
        IsReading = true;
        try
        {
            IReadOnlyList<string> extensions = Extensions;
            List<BrowserEntry> list = await Task.Run(() => Read(path, extensions));
            if (id != loadId)
            {
                return false;
            }

            all = list;
            CurrentFolder = path;
            AddressText = path;
            Message = null;
            ApplyFilter(select);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or SecurityException
                                      or ArgumentException or NotSupportedException)
        {
            if (id == loadId)
            {
                Message = $"Can't open {path}: {Reason(e)}";
                AddressText = CurrentFolder ?? "";
            }

            return false;
        }
        finally
        {
            if (id == loadId)
            {
                IsReading = false;
            }
        }
    }

    /// <summary>Up one folder, selecting the one left. False at a drive's top (the pad goes to Places then).</summary>
    public async Task<bool> UpAsync()
    {
        if (CurrentFolder is null || Directory.GetParent(CurrentFolder) is not { } parent)
        {
            return false;
        }

        return await OpenFolderAsync(parent.FullName, Path.GetFileName(Path.TrimEndingDirectorySeparator(CurrentFolder)));
    }

    /// <summary>The address field's Enter: a folder opens, a file opens its folder with the file selected.</summary>
    [RelayCommand]
    public async Task GoToAddressAsync()
    {
        AddressTarget target = ResolveAddress(AddressText, CurrentFolder);
        if (target.Error is { } error)
        {
            Message = error;
            return;
        }

        if (target.Folder is { } folder)
        {
            await OpenFolderAsync(folder, target.File is { } file ? Path.GetFileName(file) : null);
        }
    }

    /// <summary>A or Enter or a double-click on a row: a folder opens, a file of the types taken is chosen.</summary>
    public async Task ActivateAsync(BrowserEntry entry)
    {
        if (entry.IsFolder)
        {
            await OpenFolderAsync(entry.FullPath);
        }
        else if (entry.Matches)
        {
            Pick(entry.FullPath);
        }
        else
        {
            Message = $"{entry.Name} isn't a {FilterName}.";
        }
    }

    public Task<bool> OpenPlaceAsync(BrowserPlace place) => OpenFolderAsync(place.Path);

    [RelayCommand]
    private async Task ChooseAsync()
    {
        if (SelectedEntry is { } entry)
        {
            await ActivateAsync(entry);
        }
        else
        {
            Message = $"Select a file first: a {FilterName}.";
        }
    }

    [RelayCommand]
    private void Cancel() => result.TrySetResult(null);

    [RelayCommand]
    private void ToggleShowAll() => ShowAllFiles = !ShowAllFiles;

    [RelayCommand]
    private async Task UseWindowsDialogAsync()
    {
        if (windowsDialog is not null && await windowsDialog() is { } file)
        {
            Pick(file);
        }
    }

    /// <summary>
    /// Files dropped from Explorer: the first of a type taken is chosen; a dropped folder with
    /// the game's <c>default.xbe</c> is chosen by it (when this choice takes the game), another
    /// folder opens here. False: nothing taken (Message says why).
    /// </summary>
    public async Task<bool> DropAsync(IReadOnlyList<string> paths)
    {
        if (DropTarget(paths) is { } file)
        {
            Pick(file);
            return true;
        }

        if (paths.FirstOrDefault(Directory.Exists) is { } folder)
        {
            await OpenFolderAsync(folder);
            return false;
        }

        Message = DropProblem(paths);
        return false;
    }

    /// <summary>What a drop of these paths would choose, or null (the cursor shows "not allowed" then).</summary>
    public string? DropTarget(IReadOnlyList<string> paths) => DropTarget(paths, Extensions);

    /// <summary>Why a drop of these paths isn't taken (shown while it hovers).</summary>
    public string DropProblem(IReadOnlyList<string> paths) => DropProblem(paths, FilterName);

    /// <summary>Why a drop isn't taken, for a choice that takes <paramref name="filterName"/> (the Welcome pop-up's too).</summary>
    public static string DropProblem(IReadOnlyList<string> paths, string filterName) => paths.Count switch
    {
        0 => "Only files can be dropped here.",
        1 => $"{Path.GetFileName(Path.TrimEndingDirectorySeparator(paths[0]))} isn't a {filterName}.",
        _ => $"None of these {paths.Count} is a {filterName}.",
    };

    /// <summary>
    /// A drag hovering that the browser won't take: why, shown in place of the hint line ("or
    /// drag a file here"); no drop event follows a refused drag, so this is the only message.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HintText), nameof(HintIsProblem))]
    public partial string? DragProblem { get; set; }

    public string HintText => DragProblem ?? "or drag a file here";

    public bool HintIsProblem => DragProblem is not null;

    /// <summary>Would the browser take a drop of these paths: a file it takes, a game folder, or a folder to open.</summary>
    public bool TakesDrop(IReadOnlyList<string> paths) => DropTarget(paths) is not null || paths.Any(Directory.Exists);

    /// <summary>The first path of a type taken, or a dropped game folder's <c>default.xbe</c>; null if none.</summary>
    public static string? DropTarget(IReadOnlyList<string> paths, IReadOnlyList<string> extensions)
    {
        foreach (string p in paths)
        {
            if (File.Exists(p) && Matches(Path.GetFileName(p), extensions))
            {
                return p;
            }

            if (extensions.Contains(".xbe") && Directory.Exists(p) && Path.Combine(p, GameXbe) is var xbe && File.Exists(xbe))
            {
                return xbe;
            }
        }

        return null;
    }

    /// <summary>A file name of a type taken (an <c>.xbe</c> only as <c>default.xbe</c>).</summary>
    public static bool Matches(string name, IReadOnlyList<string> extensions)
    {
        string ext = Path.GetExtension(name).ToLowerInvariant();
        return extensions.Contains(ext) && (ext != ".xbe" || name.Equals(GameXbe, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// An address typed or pasted: quotes from Explorer's "Copy as path" and a <c>file:</c>
    /// link taken off, <c>%VARIABLES%</c> expanded, a bare drive letter = its top, a relative
    /// path from <paramref name="relativeTo"/>.
    /// </summary>
    public static AddressTarget ResolveAddress(string text, string? relativeTo)
    {
        string t = text.Trim();
        if (t.Length >= 2 && t[0] == '"' && t[^1] == '"')
        {
            t = t[1..^1].Trim();
        }

        if (t.StartsWith("file:", StringComparison.OrdinalIgnoreCase)
            && Uri.TryCreate(t, UriKind.Absolute, out Uri? uri) && uri.IsFile)
        {
            t = uri.LocalPath;
        }

        t = Environment.ExpandEnvironmentVariables(t);
        if (t.Length == 0)
        {
            return new(null, null, "Type or paste a folder or a file, then press Enter.");
        }

        if (t.Length == 2 && char.IsAsciiLetter(t[0]) && t[1] == ':')
        {
            t += Path.DirectorySeparatorChar;
        }

        string full;
        try
        {
            full = relativeTo is not null && !Path.IsPathFullyQualified(t) ? Path.GetFullPath(t, relativeTo) : Path.GetFullPath(t);
        }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException or SecurityException)
        {
            return new(null, null, $"\"{t}\" isn't a folder or file path.");
        }

        if (Directory.Exists(full))
        {
            return new(full, null, null);
        }

        if (File.Exists(full))
        {
            return new(Path.GetDirectoryName(full), full, null);
        }

        return new(null, null, $"Can't find {full}.");
    }

    /// <summary>Bytes as Explorer shows them (1 KB = 1,024 bytes).</summary>
    public static string SizeText(long bytes) => bytes switch
    {
        < 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes} bytes"),
        < 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{Math.Ceiling(bytes / 1024.0):0} KB"),
        < 1024L * 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / (1024.0 * 1024):0.0} MB"),
        < 1024L * 1024 * 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{bytes / (1024.0 * 1024 * 1024):0.0} GB"),
        _ => string.Create(CultureInfo.InvariantCulture, $"{bytes / (1024.0 * 1024 * 1024 * 1024):0.0} TB"),
    };

    /// <summary>
    /// Places, top to bottom: the launcher's folder, recent folders (still there), Desktop,
    /// Downloads, Documents, the drives that are ready (name, free space).
    /// </summary>
    public static List<BrowserPlace> BuildPlaces(string launcherFolder, IReadOnlyList<string> recent)
    {
        List<BrowserPlace> places = [new("Launcher folder", launcherFolder, launcherFolder, PlaceKind.Launcher)];
        foreach (string folder in recent)
        {
            if (!SamePath(folder, launcherFolder) && Directory.Exists(folder))
            {
                string name = Path.GetFileName(Path.TrimEndingDirectorySeparator(folder));
                places.Add(new(name.Length > 0 ? name : folder, folder, folder, PlaceKind.Recent));
            }
        }

        (string Label, string? Path)[] folders =
        [
            ("Desktop", Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)),
            ("Downloads", ShellFolders.Downloads),
            ("Documents", Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)),
        ];
        foreach ((string label, string? path) in folders)
        {
            if (!string.IsNullOrEmpty(path) && Directory.Exists(path))
            {
                places.Add(new(label, path, path, PlaceKind.Folder));
            }
        }

        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            try
            {
                if (!drive.IsReady)
                {
                    continue;
                }

                string letter = drive.Name.TrimEnd(Path.DirectorySeparatorChar);
                string kind = drive.DriveType switch
                {
                    DriveType.Fixed => "Local Disk",
                    DriveType.Removable => "USB Drive",
                    DriveType.Network => "Network Drive",
                    DriveType.CDRom => "Disc Drive",
                    _ => "Drive",
                };
                string name = drive.VolumeLabel is { Length: > 0 } label ? label : kind;
                places.Add(new($"{name} ({letter})", drive.RootDirectory.FullName,
                    $"{SizeText(drive.AvailableFreeSpace)} free of {SizeText(drive.TotalSize)}", PlaceKind.Drive));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or SecurityException)
            {
                // A drive that went away while asked: left out.
            }
        }

        return places;
    }

    private static List<BrowserEntry> Read(string path, IReadOnlyList<string> extensions)
    {
        DirectoryInfo dir = new(path);
        if (!dir.Exists)
        {
            throw new DirectoryNotFoundException();
        }

        // Not IgnoreInaccessible: without recursion it only hides that the folder itself can't
        // be read, which then looked like an empty folder.
        EnumerationOptions options = new()
        {
            IgnoreInaccessible = false,
            AttributesToSkip = FileAttributes.Hidden | FileAttributes.System,
        };
        List<BrowserEntry> list = [];
        foreach (FileSystemInfo info in dir.EnumerateFileSystemInfos("*", options))
        {
            bool folder = info is DirectoryInfo;
            list.Add(new(info.Name, info.FullName, folder, folder ? 0 : ((FileInfo)info).Length, info.LastWriteTime,
                !folder && Matches(info.Name, extensions)));
        }

        list.Sort((a, b) => a.IsFolder != b.IsFolder ? (a.IsFolder ? -1 : 1) : ShellFolders.CompareNames(a.Name, b.Name));
        return list;
    }

    private void ApplyFilter(string? select)
    {
        Entries = ShowAllFiles ? all : [.. all.Where(e => e.IsFolder || e.Matches)];
        SelectedEntry = select is null ? null : Entries.FirstOrDefault(e => e.Name.Equals(select, StringComparison.OrdinalIgnoreCase));
        EmptyText = Entries.Count > 0 ? null
            : all.Count > 0 ? $"No {FilterName} in this folder."
            : "This folder is empty.";
    }

    private void Pick(string file)
    {
        if (Path.GetDirectoryName(file) is { Length: > 0 } folder)
        {
            LauncherPrefs.AddRecentFolder(folder);
        }

        result.TrySetResult(file);
    }

    private static string Reason(Exception e) => e switch
    {
        UnauthorizedAccessException or SecurityException => "Windows doesn't allow reading it.",
        DirectoryNotFoundException => "it isn't there (any more).",
        _ => e.Message,
    };

    private static bool SamePath(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(a), Path.TrimEndingDirectorySeparator(b), StringComparison.OrdinalIgnoreCase);
}
