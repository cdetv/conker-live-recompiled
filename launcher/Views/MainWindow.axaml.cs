using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Input.Platform;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Avalonia.VisualTree;
using ConkerLauncher.Services;
using ConkerLauncher.ViewModels;

namespace ConkerLauncher.Views;

public sealed partial class MainWindow : Window, IFilePicker
{
    public MainWindow()
    {
        InitializeComponent();
        Opened += (_, _) => WindowFrame.Apply(this, Color.FromRgb(0x24, 0x34, 0x1F));
        // Files may have been imported or removed while the launcher was in the background.
        Activated += (_, _) =>
        {
            if (ControlsVm is not { IsCapturing: true })
            {
                (DataContext as MainWindowViewModel)?.Refresh();
            }

            if (DataContext is MainWindowViewModel vm)
            {
                vm.WindowActive = true;
            }
        };
        // The controller drives the launcher only while it is in front (to-do 121).
        Deactivated += (_, _) =>
        {
            if (DataContext is MainWindowViewModel vm)
            {
                vm.WindowActive = false;
            }
        };

        // "Press a key" (Controls tab): every key goes to the question first, before focus
        // navigation or a button can take Tab, Space or the arrows.
        AddHandler(KeyDownEvent, OnKeyDownFirst, RoutingStrategies.Tunnel);
        CaptureLayer.PointerPressed += OnCapturePointer;
        CaptureLayer.PointerWheelChanged += OnCaptureWheel;
        CaptureLayer.PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty)
            {
                OnCaptureShown(CaptureLayer.IsVisible);
            }
        };
        AddHandler(GotFocusEvent, OnFocusMoved, RoutingStrategies.Bubble, handledEventsToo: true);
        // After every layout pass, before anything is drawn: a scroll moves the ring in the same pass.
        LayoutUpdated += (_, _) => ClipFocusRing();
        // Pop-ups keep the keyboard's focus (to-do 121 part 1e).
        foreach (Control o in Overlays.Append((Control)WelcomeOverlay.Parent!))
        {
            o.PropertyChanged += (_, e) =>
            {
                if (e.Property == IsVisibleProperty)
                {
                    Dispatcher.UIThread.Post(UpdateOverlayFocus, DispatcherPriority.Loaded);
                }
            };
        }

        foreach (Control o in Overlays)
        {
            KeyboardNavigation.SetTabNavigation(o, KeyboardNavigationMode.Cycle);
        }
        // The mouse in use: the pad's help line and button hints go away (to-do 121 1f).
        AddHandler(PointerMovedEvent, OnPointerMovedAny, RoutingStrategies.Tunnel, handledEventsToo: true);
        AddHandler(PointerPressedEvent, (_, _) => EndPadMode(), RoutingStrategies.Tunnel, handledEventsToo: true);
        AddHandler(PointerWheelChangedEvent, (_, _) => EndPadMode(), RoutingStrategies.Tunnel, handledEventsToo: true);
        // Files dragged from Explorer (to-do 122 part 2d): taken by the file browser and the Welcome pop-up.
        DragDrop.SetAllowDrop(this, true);
        AddHandler(DragDrop.DragEnterEvent, OnDragOver);
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
        // A DragLeave also comes (bubbling) whenever the drag crosses from one control to the
        // next inside the window: the red line goes only when no DragOver follows (left or Esc).
        dragLeft = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        dragLeft.Tick += (_, _) =>
        {
            dragLeft.Stop();
            ClearDragProblems();
        };
        AddHandler(DragDrop.DragLeaveEvent, (_, _) => dragLeft.Start());
        AddHandler(DragDrop.DropEvent, OnDrop);
    }

    // ---- drag and drop (to-do 122 part 2d) ----------------------------------------------------

    private enum DropOn
    {
        Nothing,
        Browser,    // picked as after Choose (a folder: opened there)
        Welcome,    // the game, imported at once as after Import Files
    }

    /// <summary>
    /// Who takes a drop now: the pop-up on top, if it is the file browser or the Welcome
    /// pop-up; nothing on the page itself (a .zip / .7z could be the game or the patch).
    /// </summary>
    private DropOn DropPlace() =>
        Vm is not { } vm || vm.Controls.IsCapturing ? DropOn.Nothing
        : Overlays.FirstOrDefault(o => o.IsEffectivelyVisible) is not { } top ? DropOn.Nothing
        : top == BrowserOverlay && vm.Browser is not null ? DropOn.Browser
        : top == WelcomeOverlay ? DropOn.Welcome
        : DropOn.Nothing;

    /// <summary>The dropped items' local paths (none for items without one, such as a phone's files).</summary>
    private static List<string> DroppedPaths(DragEventArgs e) =>
        [.. (e.DataTransfer.TryGetFiles() ?? []).Select(f => f.TryGetLocalPath()).OfType<string>()];

    /// <summary>
    /// While a drag hovers: the cursor says whether it would be taken ("not allowed" else), and
    /// the pop-up's hint line says why not (a refused drag never sends a drop event).
    /// </summary>
    private readonly DispatcherTimer dragLeft;

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        dragLeft.Stop();
        List<string> paths = DroppedPaths(e);
        bool take = false;
        switch (DropPlace())
        {
            case DropOn.Browser when Vm?.Browser is { } browser:
                take = browser.TakesDrop(paths);
                browser.DragProblem = take ? null : browser.DropProblem(paths);
                break;
            case DropOn.Welcome when Vm is { } vm:
                take = FileBrowserViewModel.DropTarget(paths, GameImporter.Extensions) is not null;
                vm.WelcomeDropProblem = take ? null : FileBrowserViewModel.DropProblem(paths, FilesViewModel.GameFilterName);
                break;
        }

        e.DragEffects = take ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
        // Test log (CONKER_LAUNCHER_PAD_LOG): once per change, not every mouse move.
        string note = $"[drop] hover on {DropPlace()}: {(take ? "taken" : "refused")} ({paths.Count} item(s): {string.Join(" | ", paths)})";
        if (note != lastDragNote)
        {
            lastDragNote = note;
            Vm?.Pad.Log(note);
        }
    }

    private string? lastDragNote;

    private void ClearDragProblems()
    {
        lastDragNote = null;
        if (Vm is { } vm)
        {
            vm.WelcomeDropProblem = null;
            if (vm.Browser is { } browser)
            {
                browser.DragProblem = null;
            }
        }
    }

    private async void OnDrop(object? sender, DragEventArgs e)
    {
        List<string> paths = DroppedPaths(e);
        DropOn place = DropPlace();
        dragLeft.Stop();
        ClearDragProblems();
        e.Handled = true;
        Vm?.Pad.Log($"[drop] dropped on {place}: {string.Join(" | ", paths)}");
        switch (place)
        {
            case DropOn.Browser when Vm?.Browser is { } browser && browser.TakesDrop(paths):
                e.DragEffects = DragDropEffects.Copy;
                await browser.DropAsync(paths);     // a file: picked (the browser closes, the job starts)
                break;
            case DropOn.Welcome when Vm is { } vm && FileBrowserViewModel.DropTarget(paths, GameImporter.Extensions) is { } game:
                e.DragEffects = DragDropEffects.Copy;
                await vm.ImportDroppedAsync(game);
                break;
            default:
                e.DragEffects = DragDropEffects.None;
                break;
        }
    }

    private const double MouseSlack = 6;    // px the mouse may drift before it counts as in use

    private Border? focusedRow;
    private Control? shownOverlay;          // the pop-up on top now
    private Control? overlayOpener;         // what had the focus when it opened
    private bool overlayOpenerRing;
    private Control? beforeCapture;         // the key button "Press a key" was opened from
    private bool beforeCaptureRing;         // ... and whether its focus ring showed
    private Control? tipOwner;
    private Point? padModeMouse;            // where the mouse was when the pad took over
    private Point lastMouse;

    private MainWindowViewModel? Vm => DataContext as MainWindowViewModel;

    private ControlsViewModel? ControlsVm => (DataContext as MainWindowViewModel)?.Controls;

    /// <summary>
    /// A file choice (Import Files, Add patch): the launcher's own file browser (to-do 122), in
    /// the launcher's folder each time; its "Windows dialog..." is the Open dialog below.
    /// </summary>
    public async Task<string?> PickFileAsync(string title, string filterName, IReadOnlyList<string> patterns)
    {
        Task<string?> WindowsDialog() => WindowsDialogAsync(title, filterName, patterns);
        if (Vm is not { } vm)
        {
            return await WindowsDialog();
        }

        FileBrowserViewModel browser = new(title, filterName, patterns, vm.LauncherFolder, WindowsDialog);
        vm.Browser = browser;
        try
        {
            await browser.StartAsync();
            return await browser.Result;
        }
        finally
        {
            if (vm.Browser == browser)
            {
                vm.Browser = null;
            }
        }
    }

    /// <summary>
    /// Report Bug's buttons change with its stage: the focus goes to the main one (nothing to
    /// press while it works: the pop-up itself), never to the page under it.
    /// </summary>
    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (Vm is not { } vm)
        {
            return;
        }

        vm.Files.Report.PropertyChanged += (_, p) =>
        {
            if (p.PropertyName == nameof(ReportBugViewModel.Stage) && vm.Files.Report.IsOpen)
            {
                Dispatcher.UIThread.Post(FocusReportStage, DispatcherPriority.Loaded);
            }
        };
    }

    private void FocusReportStage()
    {
        if (Vm is not { } vm)
        {
            return;
        }

        Control target = vm.Files.Report.Stage switch
        {
            ReportBugViewModel.ReportStage.Done => ReportIssue,
            ReportBugViewModel.ReportStage.Failed => ReportBack,
            ReportBugViewModel.ReportStage.Edit => vm.PadMode ? ReportCreate : ReportText,
            _ => ReportPanel,
        };
        target.Focus(vm.PadMode ? NavigationMethod.Directional : overlayOpenerRing ? NavigationMethod.Tab : NavigationMethod.Pointer);
    }

    public async Task CopyTextAsync(string text)
    {
        if (Clipboard is { } clipboard)
        {
            await clipboard.SetTextAsync(text);
        }
    }

    /// <summary>The Windows Open dialog: the chosen file, or null.</summary>
    private async Task<string?> WindowsDialogAsync(string title, string filterName, IReadOnlyList<string> patterns)
    {
        IReadOnlyList<IStorageFile> files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = title,
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType(filterName) { Patterns = patterns }, FilePickerFileTypes.All],
        });
        return files.Count > 0 ? files[0].TryGetLocalPath() : null;
    }

    private void OnKeyDownFirst(object? sender, KeyEventArgs e)
    {
        if (ControlsVm is { IsCapturing: true } controls)
        {
            e.Handled = true;
            controls.TakeKey(e.Key);
        }
    }

    // Buttons handle their own presses, so only a press elsewhere on the question arrives here.
    private void OnCapturePointer(object? sender, PointerPressedEventArgs e)
    {
        if (ControlsVm is { IsCapturing: true } controls)
        {
            e.Handled = true;
            controls.TakeMouse(e.GetCurrentPoint(CaptureLayer).Properties.PointerUpdateKind switch
            {
                PointerUpdateKind.RightButtonPressed => MouseButton.Right,
                PointerUpdateKind.MiddleButtonPressed => MouseButton.Middle,
                PointerUpdateKind.XButton1Pressed => MouseButton.XButton1,
                PointerUpdateKind.XButton2Pressed => MouseButton.XButton2,
                _ => MouseButton.Left,
            });
        }
    }

    /// <summary>
    /// The focused option's row (<c>Border.row</c>) lights up (class <c>focused</c>, App.axaml)
    /// when the focus came from the pad or the keyboard, not from a click (to-do 121 1f).
    /// </summary>
    private void OnFocusMoved(object? sender, FocusChangedEventArgs e)
    {
        // An open dropdown's items are their own pop-up window: the row and help stay the dropdown's.
        if (e.Source is not Visual source || TopLevel.GetTopLevel(source) != this)
        {
            return;
        }

        WatchTip(e.Source as Control);
        if (e.NavigationMethod is NavigationMethod.Directional or NavigationMethod.Tab && e.Source is Control focused)
        {
            ScrollWithFocus(focused, e.OldFocusedElement as Visual);
        }


        Border? row = e.NavigationMethod is NavigationMethod.Directional or NavigationMethod.Tab
            ? (e.Source as Visual)?.GetSelfAndVisualAncestors().OfType<Border>().FirstOrDefault(b => b.Classes.Contains("row"))
            : null;
        if (row == focusedRow)
        {
            return;
        }

        focusedRow?.Classes.Remove("focused");
        focusedRow = row;
        row?.Classes.Add("focused");
    }

    /// <summary>
    /// Pad / keyboard focus in a scrolling list (user 2026-11-07): the focused option's whole row
    /// shows, and moving up also what lies above it up to the control before (its section
    /// heading, notes); the first control scrolls the list to the top, the last to the end.
    /// </summary>
    private static void ScrollWithFocus(Control focused, Visual? old)
    {
        if (focused.FindAncestorOfType<ScrollViewer>() is not { Content: Visual content } sv || !content.IsVisualAncestorOf(focused))
        {
            return;
        }

        // Positions in the list's own (extent) coordinates.
        Rect? InList(Visual v) => v.TranslatePoint(default, sv) is { } p ? new Rect(p + sv.Offset, v.Bounds.Size) : null;
        Control target = focused.GetSelfAndVisualAncestors().OfType<Border>()
            .FirstOrDefault(b => b.Classes.Contains("row") && content.IsVisualAncestorOf(b)) ?? focused;
        if (InList(target) is not { } r)
        {
            return;
        }

        double above = 0, below = sv.Extent.Height;
        bool anyAbove = false, anyBelow = false;
        foreach (Control c in content.GetVisualDescendants().OfType<Control>())
        {
            if (!c.Focusable || !c.IsEffectivelyVisible || !c.IsEffectivelyEnabled || c is ScrollViewer or SelectableTextBlock
                || c == target || target.IsVisualAncestorOf(c) || InList(c) is not { } cr)
            {
                continue;
            }

            if (cr.Bottom <= r.Top + 1)
            {
                anyAbove = true;
                above = Math.Max(above, cr.Bottom);
            }
            else if (cr.Top >= r.Bottom - 1)
            {
                anyBelow = true;
                below = Math.Min(below, cr.Top);
            }
        }

        bool up = old is not null && TopLevel.GetTopLevel(old) == TopLevel.GetTopLevel(focused)
                  && focused.TranslatePoint(default, old) is { Y: < 0 };
        double top = !anyAbove ? 0 : up ? above : r.Top;
        double bottom = !anyBelow ? sv.Extent.Height : r.Bottom;
        if (bottom - top > sv.Viewport.Height)
        {
            (top, bottom) = (r.Top, r.Bottom);
        }

        double y = sv.Offset.Y;
        if (top < y)
        {
            y = top;
        }
        else if (bottom > y + sv.Viewport.Height)
        {
            y = bottom - sv.Viewport.Height;
        }

        y = Math.Clamp(y, 0, Math.Max(0, sv.Extent.Height - sv.Viewport.Height));
        if (Math.Abs(y - sv.Offset.Y) > 0.5)
        {
            sv.SetCurrentValue(ScrollViewer.OffsetProperty, new Vector(sv.Offset.X, y));
        }
    }

    /// <summary>
    /// The focus ring is drawn on the window's top layer, not clipped to its control (so a ring
    /// round a check box shows); in a scrolling list it is clipped to the list's view top and
    /// bottom, so a control scrolled away (right stick) takes its ring with it (user 2026-11-07).
    /// Run after every layout pass (the layer clears the clip whenever it arranges, and a ring
    /// that moves with a scroll must not be drawn once unclipped). The clip is worked out from the
    /// control's place in the list: the ring's own coordinates are the control's, shifted by the
    /// ring's margin, whenever the layer moves it.
    /// </summary>
    private void ClipFocusRing()
    {
        if (FocusManager?.GetFocusedElement() is not Control f || AdornerLayer.GetAdornerLayer(f) is not { } layer)
        {
            return;
        }

        Rect? view = f.FindAncestorOfType<ScrollViewer>() is { } sv
                     && sv.TranslatePoint(default, f) is { } top
                     && sv.TranslatePoint(new Point(0, sv.Bounds.Height), f) is { } bottom
            ? new Rect(-100_000, top.Y, 200_000, bottom.Y - top.Y)
            : null;
        foreach (Control ring in layer.Children.Where(c => AdornerLayer.GetAdornedElement(c) == f))
        {
            Rect? want = view?.Translate(new Vector(0, -ring.Margin.Top));
            Rect? have = (ring.Clip as RectangleGeometry)?.Rect;
            if (want != have)
            {
                ring.Clip = want is { } w ? new RectangleGeometry(w) : null;
            }
        }
    }

    /// <summary>
    /// The help line follows the focused control's tooltip, or the nearest one around it in its
    /// row (a slider's or dropdown's is on the row); kept live (a key's clash note changes).
    /// </summary>
    private void WatchTip(Control? focused)
    {
        if (tipOwner is not null)
        {
            tipOwner.PropertyChanged -= OnTipOwnerChanged;
        }

        tipOwner = focused?.GetSelfAndVisualAncestors().OfType<Control>()
            .TakeWhile(c => c is not ScrollViewer and not TabControl and not Window)
            .FirstOrDefault(c => ToolTip.GetTip(c) is string { Length: > 0 });
        if (tipOwner is not null)
        {
            tipOwner.PropertyChanged += OnTipOwnerChanged;
        }

        if (Vm is { } vm)
        {
            vm.HelpText = tipOwner is null ? null : ToolTip.GetTip(tipOwner) as string;
        }
    }

    private void OnTipOwnerChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property == ToolTip.TipProperty && Vm is { } vm)
        {
            vm.HelpText = e.NewValue as string;
        }
    }

    private void OnPointerMovedAny(object? sender, PointerEventArgs e)
    {
        lastMouse = e.GetPosition(this);
        if (Vm is not { PadMode: true })
        {
            padModeMouse = null;
            return;
        }

        // The first move after the pad took over sets where the mouse rests; only a real move from there ends it.
        padModeMouse ??= lastMouse;
        Vector d = lastMouse - padModeMouse.Value;
        if (Math.Abs(d.X) > MouseSlack || Math.Abs(d.Y) > MouseSlack)
        {
            EndPadMode();
        }
    }

    private void EndPadMode()
    {
        padModeMouse = null;
        if (Vm is { PadMode: true } vm)
        {
            vm.PadMode = false;
        }
    }

    /// <summary>The pop-ups over the page, topmost first (as PadNavigator.TopOverlay).</summary>
    private Control[] Overlays => [UserFilesOverlay, BrowserOverlay, ReportOverlay, HistoryOverlay, UpdateOverlay, WelcomeOverlay];

    /// <summary>
    /// A pop-up showing: keyboard Tab cycles inside it and skips the page (the page stays as it
    /// looks, under the dimming); the focus goes to its safe button when something had the focus
    /// (a pop-up at start waits: the pad's first press brings the focus in), and back to that
    /// control when the last pop-up closes. Same ring as before (keyboard / pad: shown; mouse: not).
    /// </summary>
    private void UpdateOverlayFocus()
    {
        Control? top = Overlays.FirstOrDefault(o => o.IsEffectivelyVisible);
        if (top == shownOverlay)
        {
            return;
        }

        KeyboardNavigation.SetTabNavigation(Page, top is null ? KeyboardNavigationMode.Continue : KeyboardNavigationMode.None);
        Control? focused = FocusManager?.GetFocusedElement() as Control;
        if (top is not null)
        {
            if (shownOverlay is null)
            {
                overlayOpener = focused;
                overlayOpenerRing = focused?.Classes.Contains(":focus-visible") == true;
            }

            shownOverlay = top;
            if (overlayOpener is not null && (focused is null || !top.IsVisualAncestorOf(focused)))
            {
                // Report Bug: the description, to type at once; the pad, which can't type, Create Report.
                Control? into = top == ReportOverlay && ReportCreate.IsEffectivelyVisible
                                    ? (Vm?.PadMode == true ? ReportCreate : ReportText)
                                : top.GetVisualDescendants().OfType<Button>()
                                    .FirstOrDefault(b => b.Classes.Contains("safe") && b.IsEffectivelyVisible && b.IsEffectivelyEnabled)
                                ?? top.GetVisualDescendants().OfType<Control>()
                                    .FirstOrDefault(c => c.Focusable && c.IsEffectivelyVisible && c.IsEffectivelyEnabled);
                into?.Focus(overlayOpenerRing ? NavigationMethod.Tab : NavigationMethod.Pointer);
            }

            return;
        }

        shownOverlay = null;
        if (overlayOpener is { IsEffectivelyVisible: true, IsEffectivelyEnabled: true } back
            && (focused is null || !Page.IsVisualAncestorOf(focused) || focused == back))
        {
            back.Focus(overlayOpenerRing ? NavigationMethod.Tab : NavigationMethod.Pointer);
        }

        overlayOpener = null;
    }

    /// <summary>
    /// "Press a key": the focus waits on the question itself (no ring; its buttons can't take
    /// it), else the key button's ring draws on top of the question; back to that button after.
    /// </summary>
    private void OnCaptureShown(bool shown)
    {
        if (shown)
        {
            beforeCapture = FocusManager?.GetFocusedElement() as Control;
            beforeCaptureRing = beforeCapture?.Classes.Contains(":focus-visible") == true;
            CaptureLayer.Focus();
            return;
        }

        if (beforeCapture is { IsEffectivelyVisible: true } back && FocusManager?.GetFocusedElement() is null or Border { Name: "CaptureLayer" })
        {
            back.Focus(beforeCaptureRing ? NavigationMethod.Directional : NavigationMethod.Pointer);
        }

        beforeCapture = null;
    }

    private void OnCaptureWheel(object? sender, PointerWheelEventArgs e)
    {
        if (ControlsVm is { IsCapturing: true } controls)
        {
            e.Handled = true;
            controls.TakeWheel(e.Delta.Y);
        }
    }
}
