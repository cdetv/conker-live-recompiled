using System.Collections.Concurrent;
using System.Diagnostics;

namespace ConkerLauncher.Services;

/// <summary>
/// The conker.exe runs the launcher starts for itself (<c>--dump-settings</c>, <c>--list-pads</c>,
/// the <c>--pad-feed</c> helper): not the game, so <c>IsGameRunning</c> leaves them out. Before
/// (to-do 121), the 1.5 s <c>--list-pads</c> run could lock Settings and Controls for a moment.
/// </summary>
public static class OwnProcesses
{
    private static readonly ConcurrentDictionary<int, bool> Ids = new();

    /// <summary><see cref="Process.Start(ProcessStartInfo)"/>, remembered until <see cref="Forget"/>.</summary>
    public static Process? Start(ProcessStartInfo start)
    {
        Process? p = Process.Start(start);
        if (p is not null)
        {
            Ids[p.Id] = true;
        }

        return p;
    }

    /// <summary>The run has ended (call before disposing it).</summary>
    public static void Forget(Process p)
    {
        try
        {
            Ids.TryRemove(p.Id, out _);
        }
        catch (InvalidOperationException)
        {
            // Never started.
        }
    }

    public static bool Contains(int id) => Ids.ContainsKey(id);
}
