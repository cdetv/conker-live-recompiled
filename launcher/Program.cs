using System.Runtime.InteropServices;
using Avalonia;

namespace ConkerLauncher;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args) =>
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);

    // Also used by the Avalonia previewer.
    public static AppBuilder BuildAvaloniaApp()
    {
        AppBuilder builder = AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();
        // Under Wine (Proton) DirectComposition and WinUI composition are stubs: the
        // window stayed white (Wine 11.21, 2026-11-14; r-linux-proton.md R10). There the
        // window draws through a redirection surface with Avalonia's software renderer.
        if (OnWine())
            builder = builder.With(new Win32PlatformOptions
            {
                CompositionMode = [Win32CompositionMode.RedirectionSurface],
                RenderingMode = [Win32RenderingMode.Software],
            });
        return builder;
    }

    /// <summary>Wine's ntdll exports wine_get_version; Windows' does not.</summary>
    public static bool OnWine()
    {
        if (!OperatingSystem.IsWindows())
            return false;
        nint ntdll = GetModuleHandleW("ntdll.dll");
        return ntdll != 0 && GetProcAddress(ntdll, "wine_get_version") != 0;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern nint GetModuleHandleW(string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, BestFitMapping = false)]
    private static extern nint GetProcAddress(nint module, string name);
}
