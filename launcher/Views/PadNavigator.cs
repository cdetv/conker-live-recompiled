using Avalonia;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.LogicalTree;
using Avalonia.Threading;
using Avalonia.VisualTree;
using ConkerLauncher.Services;
using ConkerLauncher.ViewModels;

namespace ConkerLauncher.Views;

/// <summary>
/// The controller's buttons as launcher actions (to-do 121 part 1d; button map and the focus
/// rules from the spike in <c>docs/research/r-0.2.4-pad-launcher.md</c>). Focus moves with
/// Avalonia's <c>FindNextElement</c> and is set with <see cref="NavigationMethod.Directional"/>
/// (the focus ring shows); A acts on the focused control directly, never through fake keys,
/// so the Controls tab's "Press a key" never sees them.
/// </summary>
/// <remarks>
/// Focus follows what opens and closes: a pop-up (welcome, update, history, user files) or
/// an inline question (Delete Shaders, Reset) takes the focus on its safe button (class
/// <c>safe</c>); when it closes, the focus goes back to the control that opened it, or the
/// nearest one if that is gone or greyed out.
/// </remarks>
public sealed class PadNavigator
{
    private const double RowSlack = 24;     // Left/Right: how far off the row a target may sit
    private const double ScrollStep = 60;
    private const double RowOverlap = 8;    // more than the option rows' bands overlap (4 px)

    private readonly MainWindow window;
    private readonly MainWindowViewModel vm;
    private Control? lastActed;             // the control A was last pressed on
    private Point? lastActedAt;             // and where it was (window coordinates)
    private Control? anchor;                // what opened the pop-up or question showing now
    private Point? anchorAt;
    private long backArmedUntil;
    private Slider? adjusting;              // the slider Left / Right change (A toggles it; class "adjust")
    private Slider? aDownOn;                // A went down on this slider and is still held
    private bool aDownWasAdjusting;         // ... while it was adjusting already
    private bool movedWhileADown;           // ... and Left / Right moved it before A let go

    public PadNavigator(MainWindow window, MainWindowViewModel vm)
    {
        this.window = window;
        this.vm = vm;
        vm.Pad.Pressed += (_, e) => OnPressed(e);
        vm.Pad.Released += (_, e) => OnReleased(e);
        vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MainWindowViewModel.Browser))
            {
                OnBrowserChanged();
            }
        };
    }

    private Control? browserOpener;         // what had the focus when the file browser opened
    private Point? browserOpenerAt;

    private Control Root => (Control)window.Content!;

    private TabControl MainTabs => window.MainTabs;

    private IInputElement? Focused => window.FocusManager?.GetFocusedElement();

    private void OnPressed(PadPressEventArgs e)
    {
        vm.PadMode = true;      // help line and button hints (off when the mouse moves)
        IInputElement? before = Focused;
        double? value = (before as Slider)?.Value;
        try
        {
            Handle(e);
        }
        finally
        {
            // For PadInput's diagonal rule: did this press go anywhere?
            e.Moved = Focused != before || (before as Slider)?.Value != value;
            // After the binding updates and layout: where the focus ended up, and the hints for it.
            Dispatcher.UIThread.Post(() =>
            {
                UpdateHints();
                vm.Pad.Log($"  focus: {Describe(Focused)}"
                    + (vm.Controls.IsCapturing ? " [Press a key showing]" : TopOverlay() is { } o ? $" [{o.Name} showing]" : "")
                    + (vm.Message is { } m ? $" message: {m}" : ""));
            }, DispatcherPriority.Background);
        }
    }

    /// <summary>The button hints bar for what has the pad now and the pad's type.</summary>
    private void UpdateHints()
    {
        if (TopOverlay() == window.BrowserOverlay && vm.Browser is { } browser && !vm.Controls.IsCapturing)
        {
            // A says what it does on the focused row; X what the list will show.
            string a = Focused switch
            {
                ListBoxItem { Content: BrowserEntry { IsFolder: true } } or ListBoxItem { Content: BrowserPlace } => "Open",
                ListBoxItem { Content: BrowserEntry } => "Choose",
                _ => "Select",
            };
            vm.Hints = PadHints.ForBrowser(vm.Pad.LastKind, a, browser.ShowAllFiles);
            return;
        }

        PadHintState state = vm.Controls.IsCapturing ? PadHintState.KeyCapture
            : adjusting is not null ? PadHintState.Adjusting
            : OpenDropDown() is not null ? PadHintState.Choosing
            : TopOverlay() is not null || SafeButton(PageRoot()) is not null ? PadHintState.PopUp
            : PadHintState.Page;
        vm.Hints = PadHints.For(state, vm.Pad.LastKind);
    }

    // A let go on a slider: when it moved while A was held, holding A was the way (adjusting ends).
    private void OnReleased(PadPressEventArgs e)
    {
        if (e.Button != PadButton.A || aDownOn is not { } s)
        {
            return;
        }

        aDownOn = null;
        if (movedWhileADown && !aDownWasAdjusting && adjusting == s)
        {
            SetAdjusting(null);
            UpdateHints();
        }
    }

    private void Handle(PadPressEventArgs e)
    {
        PadButton b = e.Button;
        // B while a slider is adjusting: stop adjusting (keeps the value), nothing else.
        if (b == PadButton.B && adjusting is not null && Focused == adjusting)
        {
            SetAdjusting(null);
            return;
        }

        FileBrowserViewModel? browser = TopOverlay() == window.BrowserOverlay ? vm.Browser : null;
        if (b == PadButton.Back)
        {
            // Share / Back in the file browser closes it, not the launcher (user 2026-11-07).
            if (browser is not null)
            {
                lastActed = null;
                lastActedAt = null;
                browser.CancelCommand.Execute(null);
                AfterAction();
            }
            else
            {
                Quit();
            }

            return;
        }

        // "Press a key": its buttons can't take focus; B cancels, X = No key.
        if (vm.Controls.IsCapturing)
        {
            if (b == PadButton.B)
            {
                vm.Controls.CancelCaptureCommand.Execute(null);
                AfterAction();
            }
            else if (b == PadButton.X)
            {
                vm.Controls.NoKeyCommand.Execute(null);
                AfterAction();
            }

            return;
        }

        if (OpenDropDown() is { } box)
        {
            DropDown(box, b);
            return;
        }

        Control? overlay = TopOverlay();
        // A pop-up that opened by itself (welcome, update at start): the first press brings the
        // focus into it.
        if (overlay is not null && b != PadButton.B && !IsScroll(b)
            && (Focused is not Control f || !overlay.IsVisualAncestorOf(f) || !Allowed(f)))
        {
            Enter(overlay);
            return;
        }

        if (browser is not null && Browse(b, browser))
        {
            return;
        }

        switch (b)
        {
            case PadButton.Up or PadButton.Down or PadButton.Left or PadButton.Right:
                Direction(b, e.Held);
                break;
            case PadButton.A:
                Act();
                break;
            case PadButton.B:
                Cancel(overlay);
                break;
            case PadButton.ScrollUp or PadButton.ScrollDown or PadButton.ScrollLeft or PadButton.ScrollRight:
                Scroll(b, overlay);
                break;
            case PadButton.Y or PadButton.RT when overlay is null:
                MainTab(1);
                break;
            case PadButton.LT when overlay is null:
                MainTab(-1);
                break;
            case PadButton.RB when overlay is null:
                SubTab(1);
                break;
            case PadButton.LB when overlay is null:
                SubTab(-1);
                break;
            case PadButton.Start when overlay is null:
                Play();
                break;
        }
    }

    private static bool IsScroll(PadButton b) =>
        b is PadButton.ScrollUp or PadButton.ScrollDown or PadButton.ScrollLeft or PadButton.ScrollRight;

    // ---- moving --------------------------------------------------------------------------

    private void Direction(PadButton b, TimeSpan held)
    {
        if (Focused is not Control from || !Allowed(from) || !Root.IsVisualAncestorOf(from))
        {
            FocusFirst(PageRoot());
            return;
        }

        // A slider moves only while adjusting (A tapped) or while A is held; otherwise Left /
        // Right move the focus like anywhere else (to its reset button; user 2026-11-07).
        if (from is Slider slider && b is PadButton.Left or PadButton.Right
            && (adjusting == slider || vm.Pad.IsDown(PadButton.A)))
        {
            Nudge(slider, b == PadButton.Right ? 1 : -1, held);
            if (aDownOn == slider && vm.Pad.IsDown(PadButton.A))
            {
                movedWhileADown = true;
            }

            return;
        }

        SetAdjusting(null);

        // Up / Down in the Settings and Controls lists: row by row.
        if (b is PadButton.Up or PadButton.Down && RowOf(from) is { } fromRow
            && RowStep(from, fromRow, b == PadButton.Down) is { } inRow)
        {
            inRow.Focus(NavigationMethod.Directional);
            return;
        }

        NavigationDirection dir = b switch
        {
            PadButton.Up => NavigationDirection.Up,
            PadButton.Down => NavigationDirection.Down,
            PadButton.Left => NavigationDirection.Left,
            _ => NavigationDirection.Right,
        };
        InputElement root = (InputElement?)TopOverlay() ?? (InputElement)Root;
        // The page's scroll view first: a window-wide search skips what is scrolled out of
        // view; at its end, the window (the bottom bar).
        ScrollViewer? page = from.FindAncestorOfType<ScrollViewer>();
        if (page is not null && !root.IsVisualAncestorOf(page))
        {
            page = null;
        }

        Control? next = (page is null ? null : Find(dir, from, page)) ?? Find(dir, from, root);
        if (next is null)
        {
            return;
        }

        // Left / Right stay on the row: with nothing beside it, a check box's Right reached PLAY.
        if (dir is NavigationDirection.Left or NavigationDirection.Right && !SameRow(from, next))
        {
            return;
        }

        next.Focus(NavigationMethod.Directional);
        // Into the file browser's list from below (Show all files, the buttons): that row is chosen.
        if (next is ListBoxItem row && window.BrowserView.EntryList.IndexFromContainer(row) is >= 0 and var i)
        {
            window.BrowserView.EntryList.SelectedIndex = i;
        }
    }

    // ---- the file browser (to-do 122 part 2c) ------------------------------------------------

    /// <summary>
    /// The file browser's own buttons (brief "Button map"): A opens a folder or place / picks a
    /// file, B = up one folder (at a drive's top: to Places; never closes), X = all files or
    /// only the ones taken, LB/RB = Places &lt;-&gt; the list, LT/RT = a page, Up/Down a row
    /// at a time with the selection following, Left/Right between Places and the list.
    /// False: the usual rules (the buttons, Show all files).
    /// </summary>
    private bool Browse(PadButton b, FileBrowserViewModel browser)
    {
        FileBrowserView view = window.BrowserView;
        ListBox? list = Focused is ListBoxItem item ? item.FindAncestorOfType<ListBox>() : null;
        bool inEntries = list == view.EntryList;
        bool inPlaces = list == view.PlacesList;
        switch (b)
        {
            case PadButton.Up or PadButton.Down when list is not null:
                SetAdjusting(null);
                return ListStep(list, b == PadButton.Down ? 1 : -1, select: inEntries);
            case PadButton.Left when inEntries:
                view.FocusPlaces(ring: true);
                return true;
            case PadButton.Right when inPlaces:
                view.FocusList(ring: true);
                return true;
            case PadButton.A when Focused is ListBoxItem { Content: BrowserEntry entry } && inEntries:
                _ = browser.ActivateAsync(entry);       // a folder: the view focuses the new list
                return true;
            case PadButton.A when Focused is ListBoxItem { Content: BrowserPlace place } && inPlaces:
                _ = OpenPlace(browser, place);
                return true;
            case PadButton.B:
                _ = Up(browser);
                return true;
            case PadButton.X:
                browser.ShowAllFiles = !browser.ShowAllFiles;
                if (inEntries)
                {
                    // The list was rebuilt: the focus back on the row it was on (kept by name).
                    Dispatcher.UIThread.Post(() => view.FocusList(ring: true), DispatcherPriority.Loaded);
                }

                return true;
            case PadButton.LB or PadButton.RB:
                // From Places to the list, from the list to Places; from a button to the list.
                if (inEntries || !view.FocusList(ring: true))
                {
                    view.FocusPlaces(ring: true);
                }

                return true;
            case PadButton.LT or PadButton.RT:
                if (list is null)
                {
                    view.FocusList(ring: true);
                }
                else
                {
                    ListStep(list, b == PadButton.RT ? PageRows(list) : -PageRows(list), select: inEntries);
                }

                return true;
            default:
                return false;
        }
    }

    /// <summary>
    /// The browser closed after a pick or Cancel, in pad mode: the focus back on what opened it;
    /// opened from the Welcome pop-up (gone by then), on Import Files; that button greyed out
    /// (the import runs), on the nearest control (the job's Cancel). MainWindow's own rule
    /// covers a browser opened from the page with the keyboard or mouse.
    /// </summary>
    private void OnBrowserChanged()
    {
        if (vm.Browser is not null)
        {
            browserOpener = Focused as Control;
            browserOpenerAt = browserOpener is null ? null : Bounds(browserOpener)?.Center;
            return;
        }

        Control? opener = browserOpener;
        Point? openerAt = browserOpenerAt;
        browserOpener = null;
        browserOpenerAt = null;
        if (!vm.PadMode)
        {
            return;
        }

        // After MainWindow.UpdateOverlayFocus (Loaded) and the import's start.
        Dispatcher.UIThread.Post(() =>
        {
            if (Focused is Control f && Allowed(f) && Root.IsVisualAncestorOf(f))
            {
                return;
            }

            if (TopOverlay() is { } overlay)
            {
                Enter(overlay);
                return;
            }

            Control? back = null;
            Point? at = openerAt;
            if (opener is not null && window.WelcomeOverlay.IsVisualAncestorOf(opener))
            {
                Button? import = PageRoot()?.GetVisualDescendants().OfType<Button>().FirstOrDefault(b => b.Command == vm.Files.ImportGameCommand);
                if (import is not null && Allowed(import))
                {
                    back = import;
                }
                else if (import is not null)
                {
                    at = Bounds(import)?.Center;
                }
            }
            else if (opener is not null && Allowed(opener) && Root.IsVisualAncestorOf(opener))
            {
                back = opener;
            }

            back ??= Nearest(at);
            if (back is not null)
            {
                back.Focus(NavigationMethod.Directional);
            }
            else
            {
                FocusFirst(PageRoot());
            }

            UpdateHints();
        }, DispatcherPriority.Background);
    }

    // A place opened by the pad: on into its list (A again opens or picks there).
    private async Task OpenPlace(FileBrowserViewModel browser, BrowserPlace place)
    {
        if (await browser.OpenPlaceAsync(place))
        {
            Dispatcher.UIThread.Post(() => window.BrowserView.FocusList(ring: true), DispatcherPriority.Loaded);
        }
    }

    // B: up one folder, the one left selected and focused; at a drive's top, to Places.
    private async Task Up(FileBrowserViewModel browser)
    {
        if (await browser.UpAsync())
        {
            Dispatcher.UIThread.Post(() => window.BrowserView.FocusList(ring: true), DispatcherPriority.Loaded);
        }
        else
        {
            window.BrowserView.FocusPlaces(ring: true);
        }

        UpdateHints();
    }

    /// <summary>
    /// A row (or <paramref name="step"/> rows: a page) on in a virtualizing list (spike 2026-11-07):
    /// rows off screen don't exist, so the usual search can't reach them; focusing one brings it
    /// into view. After a right-stick scroll it goes on from the first / last row in view. False
    /// at the list's end (the usual search goes on: out to the buttons).
    /// </summary>
    private bool ListStep(ListBox list, int step, bool select)
    {
        int from = Focused is ListBoxItem li && list.IndexFromContainer(li) is >= 0 and var fi ? fi : list.SelectedIndex;
        if (Visible(list) is { } vis && (from < vis.First || from > vis.Last))
        {
            from = step > 0 ? vis.First - 1 : vis.Last + 1;
        }

        int to = Math.Abs(step) > 1 ? Math.Clamp(from + step, 0, list.ItemCount - 1) : from + step;
        if (to < 0 || to >= list.ItemCount || to == from)
        {
            return false;
        }

        if (select)
        {
            list.SelectedIndex = to;
        }

        list.ScrollIntoView(to);
        list.ContainerFromIndex(to)?.Focus(NavigationMethod.Directional);
        return true;
    }

    /// <summary>The rows wholly in view (first and last index), from the realized rows.</summary>
    private (int First, int Last)? Visible(ListBox list)
    {
        if (list.FindDescendantOfType<ScrollViewer>() is not { } sv || Bounds(sv) is not { } view)
        {
            return null;
        }

        int first = int.MaxValue, last = -1;
        foreach (Control item in list.GetRealizedContainers())
        {
            if (item.IsVisible && Bounds(item) is { } r && r.Top >= view.Top - 1 && r.Bottom <= view.Bottom + 1)
            {
                int i = list.IndexFromContainer(item);
                first = Math.Min(first, i);
                last = Math.Max(last, i);
            }
        }

        return last < 0 ? null : (first, last);
    }

    /// <summary>A page: the rows in view less one (the last row stays in view).</summary>
    private int PageRows(ListBox list) => Visible(list) is { } v ? Math.Max(1, v.Last - v.First) : 10;

    /// <summary>The option row (<c>Border.row</c>, MainWindow.axaml) a control sits in, if any.</summary>
    private static Border? RowOf(Control c) =>
        c.GetSelfAndVisualAncestors().OfType<Border>().FirstOrDefault(b => b.Classes.Contains("row"));

    /// <summary>
    /// Up / Down between option rows (user 2026-11-07: Up from a key skipped the "Invert up and
    /// down" box, because Avalonia's search prefers anything overlapping across, rows further
    /// away). First a control of the same row that way and overlapping across (a slider row's
    /// Mute above its reset button); else the nearest row that way, and in it the control
    /// nearest across (the nearer line on a tie). Null at the list's end: the usual search
    /// takes over (out to the top items, tabs or the bottom bar).
    /// </summary>
    private Control? RowStep(Control from, Border fromRow, bool down)
    {
        Visual? list = (Visual?)fromRow.FindAncestorOfType<ScrollViewer>() ?? fromRow.FindAncestorOfType<ItemsControl>();
        if (list is null || Bounds(from) is not { } f || Bounds(fromRow) is not { } fr)
        {
            return null;
        }

        List<(Control C, Border Row, Rect R)> all = [.. list.GetVisualDescendants().OfType<Control>()
            .Where(c => c != from && Allowed(c) && !from.IsVisualAncestorOf(c))
            .Select(c => (c, RowOf(c), Bounds(c)))
            .Where(x => x.Item2 is not null && x.Item3 is not null && list.IsVisualAncestorOf(x.Item2))
            .Select(x => (x.c, x.Item2!, x.Item3!.Value))];

        static double Across(Rect a, Rect b) => Math.Max(0, Math.Max(a.Left - b.Right, b.Left - a.Right));
        bool Ahead(Rect r, Rect of) => down ? r.Top >= of.Bottom - 1 : r.Bottom <= of.Top + 1;
        double Gap(Rect r, Rect of) => down ? r.Top - of.Bottom : of.Top - r.Bottom;

        (Control C, Border Row, Rect R)? same = all
            .Where(x => x.Row == fromRow && Ahead(x.R, f) && Across(x.R, f) == 0)
            .OrderBy(x => Gap(x.R, f)).Cast<(Control, Border, Rect)?>().FirstOrDefault();
        if (same is { } s)
        {
            return s.C;
        }

        // Row bands overlap a little (App.axaml Border.row: margin -2 for the highlight's room).
        bool RowAhead(Rect r, Rect of) => down ? r.Top >= of.Bottom - RowOverlap : r.Bottom <= of.Top + RowOverlap;
        List<(Border Row, Rect R)> rows = [.. all.Select(x => x.Row).Distinct()
            .Where(r => r != fromRow)
            .Select(r => (r, Bounds(r)))
            .Where(x => x.Item2 is { } rr && RowAhead(rr, fr))
            .Select(x => (x.r, x.Item2!.Value))];
        if (rows.Count == 0)
        {
            return null;
        }

        Border next = rows.OrderBy(x => Gap(x.R, fr)).First().Row;
        return all.Where(x => x.Row == next)
            .OrderBy(x => Across(x.R, f))
            .ThenBy(x => down ? x.R.Top : -x.R.Bottom)
            .Select(x => x.C)
            .FirstOrDefault();
    }

    /// <summary>The next control that way, skipping what the pad must never land on.</summary>
    private Control? Find(NavigationDirection dir, IInputElement from, InputElement root)
    {
        IFocusManager? fm = window.FocusManager;
        IInputElement? start = from;
        for (int i = 0; i < 10 && fm is not null; i++)
        {
            IInputElement? next = fm.FindNextElement(dir, new FindNextElementOptions { SearchRoot = root, FocusedElement = start });
            if (next is null || next == from)
            {
                return null;
            }

            if (next is Control c && Allowed(c))
            {
                return c;
            }

            start = next;
        }

        return null;
    }

    /// <summary>
    /// Can take the pad's focus: not a tab header (focus on one selects its tab), not the
    /// release notes' selectable text (every paragraph takes focus), not a text field (the file
    /// browser's address: a pad can't type), shown and enabled.
    /// </summary>
    private static bool Allowed(Control c) =>
        c is not TabItem and not SelectableTextBlock and not ScrollViewer and not TextBox
        && c.Focusable && c.IsEffectivelyVisible && c.IsEffectivelyEnabled;

    private bool SameRow(Control a, Control b)
    {
        if (Bounds(a) is not { } ra || Bounds(b) is not { } rb)
        {
            return false;
        }

        return rb.Bottom > ra.Top - RowSlack && rb.Top < ra.Bottom + RowSlack;
    }

    private Rect? Bounds(Control c) =>
        c.TranslatePoint(default, window) is { } p ? new Rect(p, c.Bounds.Size) : null;

    private void FocusFirst(Control? root)
    {
        root ??= Root;
        Control? first = root.GetVisualDescendants().OfType<Control>().FirstOrDefault(Allowed)
                         ?? Root.GetVisualDescendants().OfType<Control>().FirstOrDefault(Allowed);
        first?.Focus(NavigationMethod.Directional);
    }

    /// <summary>
    /// The slider Left / Right change, shown by its class <c>adjust</c> (App.axaml); ends when
    /// the focus leaves it any way (pad, mouse, keyboard).
    /// </summary>
    private void SetAdjusting(Slider? s)
    {
        if (adjusting == s)
        {
            return;
        }

        if (adjusting is { } old)
        {
            old.Classes.Remove("adjust");
            old.LostFocus -= OnAdjustingLostFocus;
        }

        adjusting = s;
        if (s is not null)
        {
            s.Classes.Add("adjust");
            s.LostFocus += OnAdjustingLostFocus;
        }

        vm.Pad.Log(s is null ? "  slider: adjusting off" : "  slider: adjusting on");
    }

    private void OnAdjustingLostFocus(object? sender, RoutedEventArgs e) => SetAdjusting(null);

    /// <summary>A slider's value by its step; held past 1 s, faster on long ones (10-400 in steps of 1).</summary>
    private static void Nudge(Slider s, int sign, TimeSpan held)
    {
        double step = s.SmallChange > 0 ? s.SmallChange : s.TickFrequency > 0 ? s.TickFrequency : 1;
        double steps = (s.Maximum - s.Minimum) / step;
        int n = held.TotalSeconds < 1 ? 1 : Math.Max(1, (int)Math.Round(steps / 40));
        s.Value = Math.Clamp(s.Value + (sign * n * step), s.Minimum, s.Maximum);
    }

    // ---- A and B --------------------------------------------------------------------------

    private void Act()
    {
        if (Focused is not Control f || !Allowed(f) || !Root.IsVisualAncestorOf(f))
        {
            FocusFirst(PageRoot());
            return;
        }

        if (f is Slider slider)
        {
            // A tap turns adjusting on or off; held with Left / Right it adjusts until let go.
            aDownOn = slider;
            aDownWasAdjusting = adjusting == slider;
            movedWhileADown = false;
            SetAdjusting(aDownWasAdjusting ? null : slider);
            return;
        }

        lastActed = f;
        lastActedAt = Bounds(f)?.Center;
        switch (f)
        {
            case ComboBox box:
                box.IsDropDownOpen = true;      // focuses the selected item
                return;
            case RadioButton radio:
                radio.IsChecked = true;
                break;
            case CheckBox check:
                check.IsChecked = check.IsChecked != true;
                break;
            case Button button:
                Invoke(button);
                break;
            default:
                return;
        }

        AfterAction();
    }

    // Through its automation peer, as a screen reader would: HyperlinkButton's link opens too.
    private static void Invoke(Button button)
    {
        if (ControlAutomationPeer.CreatePeerForElement(button).GetProvider<IInvokeProvider>() is { } invoke)
        {
            invoke.Invoke();
        }
        else if (button.Command is { } cmd && cmd.CanExecute(button.CommandParameter))
        {
            cmd.Execute(button.CommandParameter);
        }
    }

    /// <summary>B: the safe answer of the pop-up or question showing (none on the user files move).</summary>
    private void Cancel(Control? overlay)
    {
        if (SafeButton(overlay ?? PageRoot()) is { } safe)
        {
            lastActed = safe;
            lastActedAt = null;
            Invoke(safe);
            AfterAction();
        }
    }

    private static Button? SafeButton(Control? root) =>
        root?.GetVisualDescendants().OfType<Button>()
            .FirstOrDefault(b => b.Classes.Contains("safe") && b.IsEffectivelyVisible && b.IsEffectivelyEnabled);

    private void AfterAction() => Dispatcher.UIThread.Post(FixFocus, DispatcherPriority.Loaded);

    /// <summary>
    /// After A or B: into a pop-up or question that opened (its safe button), back to what
    /// opened one that closed, or the nearest control when the focused one went away.
    /// </summary>
    private void FixFocus()
    {
        Control? overlay = TopOverlay();
        Control? f = Focused as Control;
        if (overlay is not null)
        {
            if (f is null || !overlay.IsVisualAncestorOf(f) || !Allowed(f))
            {
                anchor = lastActed;
                anchorAt = lastActedAt;
                Enter(overlay);
            }

            return;
        }

        if (f is not null && Allowed(f) && Root.IsVisualAncestorOf(f))
        {
            return;
        }

        // An inline question took the place of its button: its safe answer.
        if (SafeButton(PageRoot()) is { } safe)
        {
            anchor = lastActed;
            anchorAt = lastActedAt;
            safe.Focus(NavigationMethod.Directional);
            return;
        }

        Control? back = anchor is not null && Allowed(anchor) && Root.IsVisualAncestorOf(anchor) ? anchor
            : Nearest(anchorAt ?? lastActedAt);
        anchor = null;
        anchorAt = null;
        if (back is not null)
        {
            back.Focus(NavigationMethod.Directional);
        }
        else
        {
            FocusFirst(PageRoot());
        }
    }

    private Control? Nearest(Point? at)
    {
        if (at is not { } p)
        {
            return null;
        }

        return Root.GetVisualDescendants().OfType<Control>().Where(Allowed)
            .Select(c => (c, r: Bounds(c)))
            .Where(x => x.r is not null)
            .OrderBy(x => Distance(x.r!.Value, p))
            .Select(x => x.c)
            .FirstOrDefault();
    }

    private static double Distance(Rect r, Point p)
    {
        double dx = Math.Max(Math.Max(r.Left - p.X, 0), p.X - r.Right);
        double dy = Math.Max(Math.Max(r.Top - p.Y, 0), p.Y - r.Bottom);
        return (dx * dx) + (dy * dy);
    }

    // ---- pop-ups and dropdowns -------------------------------------------------------------

    /// <summary>The pop-up on top, if one shows (the key question is handled before this).</summary>
    private Control? TopOverlay() =>
        new Control[] { window.UserFilesOverlay, window.BrowserOverlay, window.ReportOverlay, window.HistoryOverlay, window.UpdateOverlay, window.WelcomeOverlay }
            .FirstOrDefault(o => o.IsEffectivelyVisible);

    private void Enter(Control overlay)
    {
        // The file browser: its list (else Places), not Cancel.
        if (overlay == window.BrowserOverlay && (window.BrowserView.FocusList(ring: true) || window.BrowserView.FocusPlaces(ring: true)))
        {
            return;
        }

        // Report Bug: the description can't be typed with a pad, so Create Report.
        if (overlay == window.ReportOverlay && window.ReportCreate.IsEffectivelyVisible)
        {
            window.ReportCreate.Focus(NavigationMethod.Directional);
            return;
        }

        if (SafeButton(overlay) is { } safe)
        {
            safe.Focus(NavigationMethod.Directional);
        }
        else
        {
            FocusFirst(overlay);
        }
    }

    private ComboBox? OpenDropDown() =>
        Root.GetVisualDescendants().OfType<ComboBox>().FirstOrDefault(c => c.IsDropDownOpen);

    // An open dropdown is its own pop-up window: its own focus manager, item by item.
    private void DropDown(ComboBox box, PadButton b)
    {
        Control? item = Focused as ComboBoxItem;
        if (item is null || item.FindLogicalAncestorOfType<ComboBox>() != box)
        {
            item = box.ContainerFromIndex(Math.Max(box.SelectedIndex, 0));
            item?.Focus(NavigationMethod.Directional);
            if (b is not PadButton.A and not PadButton.B)
            {
                return;
            }
        }

        switch (b)
        {
            case PadButton.Up or PadButton.Down when item is not null:
                IInputElement? next = TopLevel.GetTopLevel(item)?.FocusManager?.FindNextElement(
                    b == PadButton.Up ? NavigationDirection.Up : NavigationDirection.Down);
                next?.Focus(NavigationMethod.Directional);
                break;
            case PadButton.A:
                if (item is not null && box.IndexFromContainer(item) is >= 0 and var index)
                {
                    box.SelectedIndex = index;
                }

                box.IsDropDownOpen = false;
                box.Focus(NavigationMethod.Directional);
                break;
            case PadButton.B:
                box.IsDropDownOpen = false;
                box.Focus(NavigationMethod.Directional);
                break;
        }
    }

    // ---- tabs, scrolling, Play, quit ----------------------------------------------------------

    /// <summary>The selected main tab's page.</summary>
    private Control? PageRoot() => MainTabs.SelectedContent as Control;

    private void MainTab(int step)
    {
        int n = MainTabs.ItemCount;
        MainTabs.SelectedIndex = (MainTabs.SelectedIndex + step + n) % n;
        Dispatcher.UIThread.Post(() => FocusFirst(PageRoot()), DispatcherPriority.Loaded);
    }

    /// <summary>LB / RB: Settings' and Controls' own tabs; on Files, its sections.</summary>
    private void SubTab(int step)
    {
        if (PageRoot() is not { } page)
        {
            return;
        }

        TabControl? inner = page.GetVisualDescendants().OfType<TabControl>().FirstOrDefault(t => t.Classes.Contains("inner"));
        if (inner is null)
        {
            Section(page, step);
            return;
        }

        if (!inner.IsEffectivelyEnabled || inner.ItemCount == 0)
        {
            return;
        }

        int n = inner.ItemCount;
        inner.SelectedIndex = (inner.SelectedIndex + step + n) % n;
        Dispatcher.UIThread.Post(() => FocusFirst(inner), DispatcherPriority.Loaded);
    }

    // Files: from the focused control's section to the first control of the next / previous one.
    private void Section(Control page, int step)
    {
        Visual space = (page as ScrollViewer)?.Content as Visual ?? page;
        double Y(Visual v) => v.TranslatePoint(default, space)?.Y ?? double.NaN;
        List<double> heads = [.. page.GetVisualDescendants().OfType<TextBlock>()
            .Where(t => t.Classes.Contains("section") && t.IsEffectivelyVisible).Select(Y).Where(y => !double.IsNaN(y)).Order()];
        List<(Control C, double Y)> controls = [.. page.GetVisualDescendants().OfType<Control>().Where(Allowed)
            .Select(c => (c, Y(c))).Where(x => !double.IsNaN(x.Item2)).OrderBy(x => x.Item2)];
        if (heads.Count == 0 || controls.Count == 0)
        {
            return;
        }

        double at = Focused is Control f && page.IsVisualAncestorOf(f) ? Y(f) : double.NegativeInfinity;
        int current = heads.FindLastIndex(h => h <= at);
        for (int k = 1; k <= heads.Count; k++)
        {
            int s = ((current + (step * k)) % heads.Count + heads.Count) % heads.Count;
            double top = heads[s], end = s + 1 < heads.Count ? heads[s + 1] : double.PositiveInfinity;
            if (controls.FirstOrDefault(x => x.Y > top && x.Y < end).C is { } target)
            {
                target.Focus(NavigationMethod.Directional);
                return;
            }
        }
    }

    private void Scroll(PadButton b, Control? overlay)
    {
        // The file browser: the list the focus is in (Places or the files), else the files.
        ScrollViewer? sv = overlay == window.BrowserOverlay
            ? ((Focused as Control)?.FindAncestorOfType<ListBox>() ?? window.BrowserView.EntryList).FindDescendantOfType<ScrollViewer>()
            : overlay is not null
            ? overlay.GetVisualDescendants().OfType<ScrollViewer>().FirstOrDefault(s => s.IsEffectivelyVisible)
            : (Focused as Control)?.FindAncestorOfType<ScrollViewer>() is { } own && PageRoot().IsVisualAncestorOf(own) ? own
            : PageRoot()?.GetVisualDescendants().Prepend(PageRoot()!).OfType<ScrollViewer>().FirstOrDefault(s => s.IsEffectivelyVisible);
        if (sv is null)
        {
            return;
        }

        Vector d = b switch
        {
            PadButton.ScrollUp => new Vector(0, -ScrollStep),
            PadButton.ScrollDown => new Vector(0, ScrollStep),
            PadButton.ScrollLeft => new Vector(-ScrollStep, 0),
            _ => new Vector(ScrollStep, 0),
        };
        Vector max = new(Math.Max(0, sv.Extent.Width - sv.Viewport.Width), Math.Max(0, sv.Extent.Height - sv.Viewport.Height));
        sv.Offset = new Vector(Math.Clamp(sv.Offset.X + d.X, 0, max.X), Math.Clamp(sv.Offset.Y + d.Y, 0, max.Y));
    }

    /// <summary>Start: Play; with no game files, to Files' Import Files with a note.</summary>
    private void Play()
    {
        if (vm.PlayCommand.CanExecute(null))
        {
            vm.PlayCommand.Execute(null);
            return;
        }

        if (!vm.Files.GameFilesImported)
        {
            vm.Message = "Import the game files first: Import Files, then pick your disc image.";
            MainTabs.SelectedIndex = 0;
            Dispatcher.UIThread.Post(() =>
            {
                Button? import = PageRoot()?.GetVisualDescendants().OfType<Button>()
                    .FirstOrDefault(b => b.Command == vm.Files.ImportGameCommand);
                if (import is not null && Allowed(import))
                {
                    import.Focus(NavigationMethod.Directional);
                }
            }, DispatcherPriority.Loaded);
        }
    }

    /// <summary>Back: closes the launcher; while something would be cut off, only on a second press within 3 s.</summary>
    private void Quit()
    {
        string? running = vm.Files.IsBusy ? "An import or download is running"
            : vm.IsUpdating ? "The update is running"
            : vm.UserFiles.IsBusy ? "User files are being moved"
            : null;
        long now = Environment.TickCount64;
        if (running is not null && now > backArmedUntil)
        {
            backArmedUntil = now + 3000;
            vm.Message = $"{running}: press Back again to close the launcher anyway.";
            return;
        }

        window.Close();
    }

    // ---- log -------------------------------------------------------------------------------

    private static string Describe(IInputElement? e)
    {
        if (e is not Control c)
        {
            return e?.GetType().Name ?? "nothing";
        }

        string? name = AutomationProperties.GetName(c);
        if (string.IsNullOrEmpty(name))
        {
            name = c switch
            {
                ContentControl { Content: string s } => s,
                ContentControl { Content: ChoiceOption o } => o.Name,
                ContentControl { Content: BrowserEntry be } => be.Name,
                ContentControl { Content: BrowserPlace bp } => bp.Label,
                _ => null,
            };
        }

        string state = c switch
        {
            Slider s => $" = {s.Value:0.###}",
            ComboBox b => $" = '{(b.SelectedItem as ChoiceOption)?.Name}'{(b.IsDropDownOpen ? " (open)" : "")}",
            Avalonia.Controls.Primitives.ToggleButton t => t.IsChecked == true ? " (ticked)" : " (not ticked)",
            _ => "",
        };
        return $"{c.GetType().Name} '{name}'{state}";
    }
}
