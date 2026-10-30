using System.Runtime.InteropServices;
using Avalonia.Controls;
using Avalonia.Media;

namespace ConkerLauncher.Services;

/// <summary>
/// Darkens the part of the window Windows itself draws (the outline) so the frame blends into
/// the launcher (SPEC 9.3), as in nutLauncher.
/// </summary>
/// <remarks>
/// Windows 11 takes any outline colour. Windows 10 only has a light or dark frame; its outline uses
/// the Windows accent when "Show accent colour on title bars and window borders" is on.
/// </remarks>
public static class WindowFrame
{
    private const int UseImmersiveDarkMode = 20;
    private const int BorderColor = 34;

    public static void Apply(Window window, Color border)
    {
        if (!OperatingSystem.IsWindows() || window.TryGetPlatformHandle()?.Handle is not { } handle || handle == 0)
        {
            return;
        }

        // Failures (older Windows, unsupported attribute) are harmless: the frame keeps its default.
        int darkMode = 1;
        _ = DwmSetWindowAttribute(handle, UseImmersiveDarkMode, ref darkMode, sizeof(int));
        int colorRef = border.R | (border.G << 8) | (border.B << 16);
        _ = DwmSetWindowAttribute(handle, BorderColor, ref colorRef, sizeof(int));
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(nint window, int attribute, ref int value, int size);
}
