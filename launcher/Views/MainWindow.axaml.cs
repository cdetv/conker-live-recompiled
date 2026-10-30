using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Platform.Storage;
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
        };

        // "Press a key" (Controls tab): every key goes to the question first, before focus
        // navigation or a button can take Tab, Space or the arrows.
        AddHandler(KeyDownEvent, OnKeyDownFirst, RoutingStrategies.Tunnel);
        CaptureLayer.PointerPressed += OnCapturePointer;
        CaptureLayer.PointerWheelChanged += OnCaptureWheel;
    }

    private ControlsViewModel? ControlsVm => (DataContext as MainWindowViewModel)?.Controls;

    public async Task<string?> PickFileAsync(string title, string filterName, IReadOnlyList<string> patterns)
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

    private void OnCaptureWheel(object? sender, PointerWheelEventArgs e)
    {
        if (ControlsVm is { IsCapturing: true } controls)
        {
            e.Handled = true;
            controls.TakeWheel(e.Delta.Y);
        }
    }
}
