using System.Diagnostics;
using System.Text.Json;

namespace ConkerLauncher.Services;

/// <summary>A controller the Slots tab can pick: its settings.ini value ("1", "ps2"), name and whether it is plugged in.</summary>
public sealed record Pad(string Value, string Name, bool Connected);

/// <summary>The controllers the game sees, from <c>conker.exe --list-pads</c> (SPEC 9.2), named as its F3 menu names them.</summary>
public static class PadList
{
    public static async Task<IReadOnlyList<Pad>> LoadAsync(GameFolder folder)
    {
        var start = new ProcessStartInfo(folder.GameExe, "--list-pads")
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
        };
        using Process process = Process.Start(start) ?? throw new IOException("conker.exe didn't start.");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        string json = await process.StandardOutput.ReadToEndAsync(timeout.Token);
        await process.WaitForExitAsync(timeout.Token);
        if (process.ExitCode != 0)
        {
            throw new IOException($"conker.exe --list-pads ended with {process.ExitCode}.");
        }

        using JsonDocument doc = JsonDocument.Parse(json);
        return doc.RootElement.GetProperty("pads").EnumerateArray()
            .Select(p => new Pad(p.GetProperty("value").GetString()!, p.GetProperty("name").GetString()!,
                                 p.GetProperty("connected").GetBoolean()))
            .ToList();
    }
}
