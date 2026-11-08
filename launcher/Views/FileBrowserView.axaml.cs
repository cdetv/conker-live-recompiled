using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using ConkerLauncher.ViewModels;

namespace ConkerLauncher.Views;

/// <summary>
/// The file browser pop-up (to-do 122): what the mouse and keyboard do in it. Rows: a
/// double-click or Enter opens a folder or picks a file; a click on a place opens it;
/// Backspace = up one folder; Esc = Cancel; Enter in the address field goes there. The pad's
/// rules live in <see cref="PadNavigator"/>.
/// </summary>
public sealed partial class FileBrowserView : UserControl
{
    private static readonly TimeSpan ReadingDelay = TimeSpan.FromMilliseconds(250);

    private readonly DispatcherTimer readingTimer;
    private FileBrowserViewModel? vm;
    private bool firstLoad;             // the list hasn't been filled since this browser opened
    private bool focusListAfterLoad;    // the focus was in the list when a folder was opened from it
    private bool focusRing;             // ... and its ring showed (keyboard / pad)

    public FileBrowserView()
    {
        InitializeComponent();
        readingTimer = new DispatcherTimer { Interval = ReadingDelay };
        readingTimer.Tick += (_, _) =>
        {
            readingTimer.Stop();
            ReadingLine.IsVisible = vm is { IsReading: true };
        };
        EntryList.DoubleTapped += OnEntryDoubleTapped;
        PlacesList.Tapped += OnPlaceTapped;
        AddressBox.AddHandler(KeyDownEvent, OnAddressKey, RoutingStrategies.Tunnel);
        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Bubble);
    }

    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (vm is not null)
        {
            vm.PropertyChanged -= OnVmChanged;
        }

        vm = DataContext as FileBrowserViewModel;
        firstLoad = true;
        focusListAfterLoad = false;
        readingTimer.Stop();
        ReadingLine.IsVisible = false;
        if (vm is not null)
        {
            vm.PropertyChanged += OnVmChanged;
            SyncPlace();
        }
    }

    private void OnVmChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(FileBrowserViewModel.IsReading) when vm is not null:
                if (vm.IsReading)
                {
                    // Opened from the list (Enter, a double-click): the list keeps the focus after.
                    Control? f = TopLevel.GetTopLevel(this)?.FocusManager?.GetFocusedElement() as Control;
                    focusListAfterLoad = f is not null && (f == EntryList || EntryList.IsVisualAncestorOf(f));
                    focusRing = f?.Classes.Contains(":focus-visible") == true;
                    readingTimer.Start();
                }
                else
                {
                    readingTimer.Stop();
                    ReadingLine.IsVisible = false;
                }

                break;
            case nameof(FileBrowserViewModel.Entries):
                Dispatcher.UIThread.Post(AfterLoad, DispatcherPriority.Loaded);
                break;
            case nameof(FileBrowserViewModel.CurrentPlace) or nameof(FileBrowserViewModel.Places):
                // After the list's own binding has the new places (CurrentPlace changes before Places is announced).
                Dispatcher.UIThread.Post(SyncPlace, DispatcherPriority.Loaded);
                break;
        }
    }

    /// <summary>A new list: its top (or the selected row) in view; the focus back in it when it was there.</summary>
    private void AfterLoad()
    {
        if (vm is null)
        {
            return;
        }

        int index = vm.SelectedEntry is { } sel ? IndexOf(vm.Entries, sel) : -1;
        if (vm.Entries.Count > 0)
        {
            EntryList.ScrollIntoView(Math.Max(index, 0));
        }

        if (firstLoad)
        {
            // Just opened: the pop-up's focus (MainWindow puts it on Cancel) moves to the list.
            firstLoad = false;
            Control? f = TopLevel.GetTopLevel(this)?.FocusManager?.GetFocusedElement() as Control;
            if (f is not null && this.IsVisualAncestorOf(f))
            {
                bool ring = f.Classes.Contains(":focus-visible");
                _ = FocusRow(index, ring) || (ring && FocusPlaces(ring));
            }

            return;
        }

        if (focusListAfterLoad)
        {
            // An empty folder (keyboard / pad): Places, so the focus isn't lost.
            focusListAfterLoad = false;
            _ = FocusRow(index, focusRing) || (focusRing && FocusPlaces(focusRing));
        }
    }

    /// <summary>
    /// The focus on a row of the list (<paramref name="index"/>, else the first); none when it is
    /// empty. With the ring (keyboard / pad) the row is selected too: Choose takes the row the
    /// focus is on (to-do 122 spike: selection follows the pad).
    /// </summary>
    public bool FocusRow(int index, bool ring)
    {
        if (vm is null || vm.Entries.Count == 0)
        {
            return false;
        }

        index = Math.Clamp(index, 0, vm.Entries.Count - 1);
        if (ring)
        {
            EntryList.SelectedIndex = index;
        }

        EntryList.ScrollIntoView(index);
        if (EntryList.ContainerFromIndex(index) is not { } row)
        {
            return false;
        }

        row.Focus(ring ? NavigationMethod.Directional : NavigationMethod.Pointer);
        return true;
    }

    /// <summary>The focus on the list's selected row, else its first (the pad: LB/RB, Right from Places, after B).</summary>
    public bool FocusList(bool ring) =>
        vm is not null && FocusRow(vm.SelectedEntry is { } sel ? IndexOf(vm.Entries, sel) : 0, ring);

    /// <summary>The focus on Places: the folder listed when it is a place, else the first (LB/RB, Left from the list, B at a drive's top).</summary>
    public bool FocusPlaces(bool ring)
    {
        if (PlacesList.ItemCount == 0)
        {
            return false;
        }

        int index = Math.Max(PlacesList.SelectedIndex, 0);
        PlacesList.ScrollIntoView(index);
        if (PlacesList.ContainerFromIndex(index) is not { } row)
        {
            return false;
        }

        row.Focus(ring ? NavigationMethod.Directional : NavigationMethod.Pointer);
        return true;
    }

    private static int IndexOf(IReadOnlyList<BrowserEntry> list, BrowserEntry e)
    {
        for (int i = 0; i < list.Count; i++)
        {
            if (ReferenceEquals(list[i], e))
            {
                return i;
            }
        }

        return -1;
    }

    // Places shows the folder listed (a click selects first; a place that couldn't open goes back).
    private void SyncPlace()
    {
        if (vm is not null && !ReferenceEquals(PlacesList.SelectedItem, vm.CurrentPlace))
        {
            PlacesList.SelectedItem = vm.CurrentPlace;
        }
    }

    private async void OnPlaceTapped(object? sender, TappedEventArgs e)
    {
        if (vm is not null && ItemOf(e.Source) is BrowserPlace place)
        {
            await vm.OpenPlaceAsync(place);
            SyncPlace();
        }
    }

    private async void OnEntryDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (vm is not null && ItemOf(e.Source) is BrowserEntry entry)
        {
            await vm.ActivateAsync(entry);
        }
    }

    /// <summary>The row's item under a click (not the scroll bar's).</summary>
    private static object? ItemOf(object? source) =>
        (source as Visual)?.GetSelfAndVisualAncestors().OfType<ListBoxItem>().FirstOrDefault()?.DataContext;

    // Enter in the address field: go there; the list takes the focus when it led somewhere.
    private async void OnAddressKey(object? sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter || vm is null)
        {
            return;
        }

        e.Handled = true;
        await vm.GoToAddressCommand.ExecuteAsync(null);
        if (vm.Message is null)
        {
            int index = vm.SelectedEntry is { } sel ? IndexOf(vm.Entries, sel) : 0;
            FocusRow(index, ring: true);
        }
    }

    private async void OnKey(object? sender, KeyEventArgs e)
    {
        if (vm is null || e.Handled || e.KeyModifiers != KeyModifiers.None)
        {
            return;
        }

        Control? f = TopLevel.GetTopLevel(this)?.FocusManager?.GetFocusedElement() as Control;
        bool inAddress = f == AddressBox;
        switch (e.Key)
        {
            case Key.Escape:
                e.Handled = true;
                vm.CancelCommand.Execute(null);
                break;
            case Key.Back when !inAddress:
                e.Handled = true;
                await vm.UpAsync();
                break;
            case Key.Enter when f is not null && PlacesList.IsVisualAncestorOf(f) && PlacesList.SelectedItem is BrowserPlace place:
                e.Handled = true;
                await vm.OpenPlaceAsync(place);
                SyncPlace();
                break;
            case Key.Enter when f is not null && EntryList.IsVisualAncestorOf(f) && vm.SelectedEntry is { } entry:
                e.Handled = true;
                await vm.ActivateAsync(entry);
                break;
        }
    }
}
