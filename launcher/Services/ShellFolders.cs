using System.Runtime.InteropServices;

namespace ConkerLauncher.Services;

/// <summary>
/// What .NET doesn't give the file browser (to-do 122): the Downloads folder (no
/// <see cref="Environment.SpecialFolder"/> for it) and Explorer's name order ("disc 2"
/// before "disc 10").
/// </summary>
public static class ShellFolders
{
    private static readonly Guid DownloadsId = new("374DE290-123F-4565-9164-39C4925E467B");

    /// <summary>The user's Downloads folder (where Windows has it, moved or not), or null.</summary>
    public static string? Downloads
    {
        get
        {
            if (!OperatingSystem.IsWindows())
            {
                return null;
            }

            Guid id = DownloadsId;
            if (SHGetKnownFolderPath(ref id, 0, 0, out nint path) != 0)
            {
                Marshal.FreeCoTaskMem(path);
                return null;
            }

            try
            {
                return Marshal.PtrToStringUni(path);
            }
            finally
            {
                Marshal.FreeCoTaskMem(path);
            }
        }
    }

    /// <summary>Explorer's order: numbers inside names compare as numbers, case ignored.</summary>
    public static int CompareNames(string a, string b) =>
        OperatingSystem.IsWindows() ? StrCmpLogicalW(a, b) : string.Compare(a, b, StringComparison.OrdinalIgnoreCase);

    [DllImport("shell32.dll")]
    private static extern int SHGetKnownFolderPath(ref Guid id, uint flags, nint token, out nint path);

    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
    private static extern int StrCmpLogicalW(string a, string b);
}
