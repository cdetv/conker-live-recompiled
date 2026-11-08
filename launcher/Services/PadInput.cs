using System.Diagnostics;
using System.Globalization;
using System.Text;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace ConkerLauncher.Services;

/// <summary>A controller button as the launcher uses it (to-do 121). The D-pad and the left stick are one set of directions; the right stick scrolls.</summary>
public enum PadButton
{
    Up, Down, Left, Right,
    A, B, X, Y,
    LB, RB, LT, RT,
    Start, Back, LeftStick, RightStick,
    ScrollUp, ScrollDown, ScrollLeft, ScrollRight,
}

/// <summary>Which button names to show: Xbox letters or PlayStation shapes.</summary>
public enum PadKind
{
    Xbox,
    PlayStation,
}

/// <summary>A press, or a held direction's repeat; <see cref="Held"/> = how long it has been held.</summary>
public sealed class PadPressEventArgs(PadButton button, bool isRepeat, TimeSpan held) : EventArgs
{
    public PadButton Button { get; } = button;

    public bool IsRepeat { get; } = isRepeat;

    public TimeSpan Held { get; } = held;

    /// <summary>
    /// Set by the handler: the press moved the focus (or a slider). A second direction
    /// pressed just after it, with this one still held, is then a diagonal's other half
    /// and gets no press of its own.
    /// </summary>
    public bool Moved { get; set; }
}

/// <summary>
/// The launcher's controller input (to-do 121, SPEC 9.3): <c>conker.exe --pad-feed</c> run as a
/// hidden helper (SPEC 9.2), so the launcher reads the pads with the game's own code (XInput,
/// SDL3 for PlayStation pads, the game's button mapping). Every connected controller drives the
/// launcher. <see cref="Pressed"/> comes on the UI thread, only while <see cref="Accepting"/>.
/// </summary>
/// <remarks>
/// A held direction repeats after 400 ms, then every 80 ms; of several held directions the
/// newest counts, except a diagonal's second half (within 100 ms of a press that moved
/// something), which is held without a press. A button already held when <see cref="Accepting"/> turns on counts only once
/// let go and pressed again. <c>CONKER_LAUNCHER_PAD_FILE=&lt;file&gt;</c> (tests) replaces the
/// helper with lines appended to that file: <c>&lt;button&gt;[:&lt;ms&gt;]</c> taps or holds a
/// button (<c>a</c>, <c>lt</c>, <c>down</c>, <c>lleft</c> = left stick, <c>rup</c> = right stick,
/// <see cref="FileButtons"/>), <c>+a</c> / <c>-a</c> holds a button until let go (taps in between
/// press on top of it), <c>kind ps</c> / <c>kind xbox</c> sets the pad's type; it works
/// whether or not the window is in front. <c>CONKER_LAUNCHER_PAD_LOG=&lt;file&gt;</c> writes what
/// arrives and what it became.
/// </remarks>
public sealed partial class PadInput : ObservableObject, IDisposable
{
    // --pad-feed's bits (src/padfeed.c): XInput's buttons, then the triggers and sticks.
    private static readonly (PadButton Button, uint Bits)[] Map =
    [
        (PadButton.Up, 0x0001 | 0x040000), (PadButton.Down, 0x0002 | 0x080000),
        (PadButton.Left, 0x0004 | 0x100000), (PadButton.Right, 0x0008 | 0x200000),
        (PadButton.Start, 0x0010), (PadButton.Back, 0x0020),
        (PadButton.LeftStick, 0x0040), (PadButton.RightStick, 0x0080),
        (PadButton.LB, 0x0100), (PadButton.RB, 0x0200),
        (PadButton.A, 0x1000), (PadButton.B, 0x2000), (PadButton.X, 0x4000), (PadButton.Y, 0x8000),
        (PadButton.LT, 0x10000), (PadButton.RT, 0x20000),
        (PadButton.ScrollUp, 0x400000), (PadButton.ScrollDown, 0x800000),
        (PadButton.ScrollLeft, 0x1000000), (PadButton.ScrollRight, 0x2000000),
    ];

    /// <summary>The test file's button names and their --pad-feed bits.</summary>
    public static readonly IReadOnlyDictionary<string, uint> FileButtons = new Dictionary<string, uint>
    {
        ["up"] = 0x1, ["down"] = 0x2, ["left"] = 0x4, ["right"] = 0x8,
        ["start"] = 0x10, ["back"] = 0x20, ["l3"] = 0x40, ["r3"] = 0x80,
        ["lb"] = 0x100, ["rb"] = 0x200, ["a"] = 0x1000, ["b"] = 0x2000, ["x"] = 0x4000, ["y"] = 0x8000,
        ["lt"] = 0x10000, ["rt"] = 0x20000,
        ["lup"] = 0x40000, ["ldown"] = 0x80000, ["lleft"] = 0x100000, ["lright"] = 0x200000,
        ["rup"] = 0x400000, ["rdown"] = 0x800000, ["rleft"] = 0x1000000, ["rright"] = 0x2000000,
    };

    private static readonly PadButton[] Directions = [PadButton.Up, PadButton.Down, PadButton.Left, PadButton.Right];

    private static readonly PadButton[] Scrolls = [PadButton.ScrollUp, PadButton.ScrollDown, PadButton.ScrollLeft, PadButton.ScrollRight];

    private const int RepeatDelayMs = 400;
    private const int RepeatEveryMs = 80;
    private const int TapMs = 60;
    private const int DiagonalMs = 100; // a diagonal's two halves: 9-84 ms apart (user's DualShock 4)
    private const int FilePad = 8;      // the test file's pad: after the 8 host pads

    private readonly GameFolder folder;
    private readonly string? padFile = Environment.GetEnvironmentVariable("CONKER_LAUNCHER_PAD_FILE") is { Length: > 0 } f ? f : null;
    private readonly string? logFile = Environment.GetEnvironmentVariable("CONKER_LAUNCHER_PAD_LOG") is { Length: > 0 } l ? l : null;
    private readonly Lock logLock = new();
    private PadKind fileKind;           // the test file's task only
    private uint fileHeld;              // the test file's "+<button>" lines

    // UI thread only from here on.
    private readonly Dictionary<int, uint> masks = [];
    private readonly Dictionary<int, PadKind> kinds = [];
    private readonly HashSet<PadButton> down = [];
    private readonly HashSet<PadButton> latched = [];
    private readonly RepeatGroup moves = new(Directions);
    private readonly RepeatGroup scrolls = new(Scrolls);
    private readonly DispatcherTimer repeatTimer = new() { Interval = TimeSpan.FromMilliseconds(15) };
    private Process? helper;
    private long helperStarted;
    private CancellationTokenSource? fileJob;
    private bool accepting;
    private PadButton? lastMove;        // the last direction press that moved something
    private long lastMoveAt;            // ... and when (Environment.TickCount64)

    public PadInput(GameFolder folder)
    {
        this.folder = folder;
        repeatTimer.Tick += (_, _) => Repeat();
    }

    /// <summary>A press or repeat while <see cref="Accepting"/> (UI thread).</summary>
    public event EventHandler<PadPressEventArgs>? Pressed;

    /// <summary>A button let go whose press went out (UI thread); a slider's "hold A" ends on A's.</summary>
    public event EventHandler<PadPressEventArgs>? Released;

    /// <summary>The button is held now and its press went out (not one held since before <see cref="Accepting"/>).</summary>
    public bool IsDown(PadButton button) => accepting && down.Contains(button) && !latched.Contains(button);

    /// <summary>The type of the pad that last pressed something (button hints).</summary>
    [ObservableProperty]
    public partial PadKind LastKind { get; set; }

    partial void OnLastKindChanged(PadKind value) => Log($"pad type now {value}");

    /// <summary>The test file stands in for the helper (<c>CONKER_LAUNCHER_PAD_FILE</c>).</summary>
    public bool FromFile => padFile is not null;

    /// <summary>
    /// Presses go out: the window is in front and no game, precompile run or update is going
    /// (the window decides). Turning on latches what is held already.
    /// </summary>
    public bool Accepting
    {
        get => accepting;
        set
        {
            if (accepting == value)
            {
                return;
            }

            accepting = value;
            latched.Clear();
            if (value)
            {
                latched.UnionWith(down);
            }

            moves.Clear();
            scrolls.Clear();
            lastMove = null;
            repeatTimer.Stop();
            Log(value ? $"accepting (latched: {string.Join(" ", latched)})" : "not accepting");
        }
    }

    /// <summary>Starts the helper (or the test file); nothing if it runs already.</summary>
    public void Start()
    {
        if (padFile is not null)
        {
            if (fileJob is null)
            {
                fileJob = new CancellationTokenSource();
                CancellationToken cancel = fileJob.Token;
                _ = Task.Run(() => ReadFileAsync(padFile, cancel), cancel);
                Log($"reading {padFile}");
            }

            return;
        }

        if (helper is not null || !File.Exists(folder.GameExe))
        {
            return;
        }

        var start = new ProcessStartInfo(folder.GameExe, "--pad-feed")
        {
            WorkingDirectory = folder.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            // stderr ([INPUT] lines) not redirected: unread, a full pipe would stall the helper.
        };
        Process? p;
        try
        {
            p = OwnProcesses.Start(start);      // not the game (IsGameRunning)
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            Log($"helper didn't start: {e.Message}");
            return;
        }

        if (p is null)
        {
            return;
        }

        helper = p;
        helperStarted = Environment.TickCount64;
        Log($"helper started, pid {p.Id}");
        _ = Task.Run(() => ReadHelperAsync(p));
    }

    /// <summary>
    /// Stops the helper and waits until it is gone (conker.exe is free to replace or start),
    /// and lets go of everything held.
    /// </summary>
    public void Stop()
    {
        fileJob?.Cancel();
        fileJob?.Dispose();
        fileJob = null;
        if (helper is { } p)
        {
            helper = null;
            try
            {
                p.StandardInput.Close();        // the helper quits when its input closes
                if (!p.WaitForExit(1000))
                {
                    p.Kill();
                    p.WaitForExit(1000);
                }
            }
            catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception or IOException)
            {
                // Already gone.
            }

            Log("helper stopped");
        }

        masks.Clear();
        kinds.Clear();
        Update();
    }

    /// <summary>The window closed: <see cref="Stop"/>.</summary>
    public void Dispose() => Stop();

    private async Task ReadHelperAsync(Process p)
    {
        bool ok = false;
        try
        {
            string? first = await p.StandardOutput.ReadLineAsync();
            ok = first == "pad-feed 1";
            if (!ok)
            {
                // An older conker.exe ends at once with an unknown argument.
                Log($"helper said \"{first}\": no controller input");
                return;
            }

            while (await p.StandardOutput.ReadLineAsync() is { } line)
            {
                Log($"< {line}");
                Parse(line);
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or InvalidOperationException)
        {
            // Stopped.
        }
        finally
        {
            Dispatcher.UIThread.Post(() => HelperEnded(p, ok));
        }
    }

    // "pad <n> <xbox|ps> <hex>" and "conn <n> <0|1>" (SPEC 9.2).
    private void Parse(string line)
    {
        string[] w = line.Split(' ');
        if (w.Length == 4 && w[0] == "pad" && int.TryParse(w[1], CultureInfo.InvariantCulture, out int n)
            && uint.TryParse(w[3], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint mask))
        {
            PadKind kind = w[2] == "ps" ? PadKind.PlayStation : PadKind.Xbox;
            Dispatcher.UIThread.Post(() => SetPad(n, kind, mask));
        }
        else if (w.Length == 3 && w[0] == "conn" && w[2] == "0" && int.TryParse(w[1], CultureInfo.InvariantCulture, out int gone))
        {
            Dispatcher.UIThread.Post(() => SetPad(gone, PadKind.Xbox, 0));
        }
    }

    private void HelperEnded(Process p, bool ok)
    {
        bool unexpected = helper == p;
        long lived = Environment.TickCount64 - helperStarted;
        OwnProcesses.Forget(p);
        p.Dispose();
        if (!unexpected)
        {
            return;
        }

        helper = null;
        masks.Clear();
        kinds.Clear();
        Update();
        Log($"helper ended by itself after {lived} ms");
        // A helper that ran a while and died gets another go; one that can't start doesn't loop.
        if (ok && lived > 5000)
        {
            Start();
        }
    }

    private async Task ReadFileAsync(string path, CancellationToken cancel)
    {
        long pos = File.Exists(path) ? new FileInfo(path).Length : 0;     // only lines added from now on
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                string text = "";
                try
                {
                    using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    if (stream.Length < pos)
                    {
                        pos = 0;        // made again
                    }

                    stream.Seek(pos, SeekOrigin.Begin);
                    var bytes = new byte[stream.Length - pos];
                    int got = await stream.ReadAtLeastAsync(bytes, bytes.Length, throwOnEndOfStream: false, cancel);
                    int end = got > 0 ? Array.LastIndexOf(bytes, (byte)'\n', got - 1) : -1;
                    if (end >= 0)
                    {
                        text = Encoding.UTF8.GetString(bytes, 0, end + 1);
                        pos += end + 1;
                    }
                }
                catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
                {
                    pos = 0;
                }
                catch (IOException)
                {
                    // Being written: next time.
                }

                foreach (string raw in text.Split('\n'))
                {
                    await FileLineAsync(raw.Trim().ToLowerInvariant(), cancel);
                }

                await Task.Delay(50, cancel);
            }
        }
        catch (OperationCanceledException)
        {
            // Stopped.
        }
    }

    private async Task FileLineAsync(string line, CancellationToken cancel)
    {
        if (line.Length == 0)
        {
            return;
        }

        Log($"< file: {line}");
        if (line is "kind ps" or "kind xbox")
        {
            fileKind = line == "kind ps" ? PadKind.PlayStation : PadKind.Xbox;
            return;
        }

        PadKind kind = fileKind;
        // "+a" holds a button down until "-a" (taps in between press on top of it).
        if (line[0] is '+' or '-' && FileButtons.TryGetValue(line[1..], out uint holdBits))
        {
            fileHeld = line[0] == '+' ? fileHeld | holdBits : fileHeld & ~holdBits;
            uint held = fileHeld;
            Dispatcher.UIThread.Post(() => SetPad(FilePad, kind, held));
            await Task.Delay(TapMs, cancel);
            return;
        }

        string[] parts = line.Split(':');
        int ms = TapMs;
        if (!FileButtons.TryGetValue(parts[0], out uint bits)
            || (parts.Length > 1 && !int.TryParse(parts[1], CultureInfo.InvariantCulture, out ms)))
        {
            Log($"unknown pad file line \"{line}\"");
            return;
        }

        uint under = fileHeld;
        Dispatcher.UIThread.Post(() => SetPad(FilePad, kind, under | bits));
        await Task.Delay(Math.Max(ms, 1), cancel);
        Dispatcher.UIThread.Post(() => SetPad(FilePad, kind, under));
        await Task.Delay(TapMs, cancel);      // released for a moment before the next line
    }

    private void SetPad(int pad, PadKind kind, uint mask)
    {
        uint before = masks.GetValueOrDefault(pad);
        if (mask == 0)
        {
            masks.Remove(pad);
        }
        else
        {
            masks[pad] = mask;
            kinds[pad] = kind;
        }

        if ((mask & ~before) != 0)
        {
            LastKind = kind;
        }

        Update();
    }

    // All pads together: presses and releases since the last change.
    private void Update()
    {
        uint all = 0;
        foreach (uint m in masks.Values)
        {
            all |= m;
        }

        List<PadButton> pressed = [];
        foreach ((PadButton button, uint bits) in Map)
        {
            bool held = (all & bits) != 0;
            if (held && down.Add(button))
            {
                pressed.Add(button);
            }
            else if (!held && down.Remove(button))
            {
                Release(button);
            }
        }

        // A stick pushed straight to a diagonal sets two directions in one report (user's
        // DualShock 4, 2026-11-07): one press, up/down first (Map order; lists run down the
        // page); the other is held without one and takes over if the first is let go.
        bool moved = false, scrolled = false;
        foreach (PadButton button in pressed)
        {
            if ((moved && moves.Has(button)) || (scrolled && scrolls.Has(button)))
            {
                HoldQuietly(button);
                continue;
            }

            if (Press(button))
            {
                moved |= moves.Has(button);
                scrolled |= scrolls.Has(button);
            }
        }
    }

    private void HoldQuietly(PadButton button)
    {
        if (accepting && !latched.Contains(button))
        {
            (moves.Has(button) ? moves : scrolls).HeldBehind(button);
            Log($"{button} with another direction: held without a press");
        }
    }

    // True when the press went out.
    private bool Press(PadButton button)
    {
        if (!accepting)
        {
            return false;
        }

        if (latched.Contains(button))
        {
            Log($"{button} held since before: ignored until let go");
            return false;
        }

        // A diagonal often comes in two reports (user's DualShock 4, 2026-11-07: Right, then
        // Right + Down 9-84 ms later, D-pad and stick alike). When the first half moved the
        // focus and is still held, the second is held without a press, so the push moves once;
        // when the first found nothing that way, the second gets its press.
        long now = Environment.TickCount64;
        if (moves.Has(button) && lastMove is { } first && first != button
            && down.Contains(first) && !latched.Contains(first) && now - lastMoveAt < DiagonalMs)
        {
            moves.HeldBehind(button);
            Log($"{button} {now - lastMoveAt} ms after {first}: a diagonal, held without a press");
            return false;
        }

        if (moves.Has(button) || scrolls.Has(button))
        {
            (moves.Has(button) ? moves : scrolls).Pressed(button);
            repeatTimer.Start();
        }

        PadPressEventArgs press = Raise(button, false, TimeSpan.Zero);
        if (moves.Has(button))
        {
            lastMove = press.Moved ? button : null;
            lastMoveAt = now;
        }

        return true;
    }

    private void Release(PadButton button)
    {
        bool wasLatched = latched.Remove(button);
        if (accepting && !wasLatched)
        {
            Log($"release {button}");
            Released?.Invoke(this, new PadPressEventArgs(button, false, TimeSpan.Zero));
        }

        RepeatGroup? group = moves.Has(button) ? moves : scrolls.Has(button) ? scrolls : null;
        if (group is null || !accepting)
        {
            return;
        }

        group.Released(button, down, latched);
        if (moves.Current is null && scrolls.Current is null)
        {
            repeatTimer.Stop();
        }
    }

    private void Repeat()
    {
        if (!accepting)
        {
            repeatTimer.Stop();
            return;
        }

        foreach (RepeatGroup group in (RepeatGroup[])[moves, scrolls])
        {
            if (group.Due() is { } button)
            {
                Raise(button, true, group.HeldFor);
            }
        }
    }

    private PadPressEventArgs Raise(PadButton button, bool repeat, TimeSpan held)
    {
        Log(repeat ? $"repeat {button} ({held.TotalMilliseconds:0} ms)" : $"press {button}");
        PadPressEventArgs e = new(button, repeat, held);
        Pressed?.Invoke(this, e);
        return e;
    }

    /// <summary>A line in <c>CONKER_LAUNCHER_PAD_LOG</c> (the navigator's too); nothing without it.</summary>
    public void Log(string text)
    {
        if (logFile is null)
        {
            return;
        }

        lock (logLock)
        {
            try
            {
                File.AppendAllText(logFile, $"{DateTime.Now:HH:mm:ss.fff} {text}\r\n");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // A log line lost.
            }
        }
    }

    /// <summary>
    /// One set of directions that repeat while held: the newest held one counts. When it is let
    /// go and another is still held, that one takes over without a press of its own.
    /// </summary>
    private sealed class RepeatGroup(PadButton[] buttons)
    {
        private readonly List<PadButton> order = [];    // held ones, oldest first
        private long since;
        private long next;

        public PadButton? Current { get; private set; }

        public TimeSpan HeldFor => TimeSpan.FromMilliseconds(Environment.TickCount64 - since);

        public bool Has(PadButton button) => buttons.Contains(button);

        public void Pressed(PadButton button)
        {
            order.Remove(button);
            order.Add(button);
            Current = button;
            since = Environment.TickCount64;
            next = since + RepeatDelayMs;
        }

        /// <summary>Held, but behind the current one (no press of its own).</summary>
        public void HeldBehind(PadButton button)
        {
            order.Remove(button);
            order.Insert(Math.Max(0, order.Count - 1), button);
        }

        public void Released(PadButton button, HashSet<PadButton> down, HashSet<PadButton> latched)
        {
            order.Remove(button);
            order.RemoveAll(b => !down.Contains(b) || latched.Contains(b));
            if (Current != button)
            {
                return;
            }

            Current = order.Count > 0 ? order[^1] : null;
            since = Environment.TickCount64;
            next = since + RepeatDelayMs;
        }

        public PadButton? Due()
        {
            long now = Environment.TickCount64;
            if (Current is null || now < next)
            {
                return null;
            }

            // From the last due time, so the timer's ~16 ms steps don't stretch every repeat;
            // after a stall (UI busy), from now instead of a burst.
            next += RepeatEveryMs;
            if (next <= now)
            {
                next = now + RepeatEveryMs;
            }

            return Current;
        }

        public void Clear()
        {
            order.Clear();
            Current = null;
        }
    }
}
