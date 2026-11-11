using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using ConkerLauncher.Services;
using ConkerLauncher.ViewModels;
using ConkerLauncher.Views;

namespace ConkerLauncher;

public sealed partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var window = new MainWindow();
            var viewModel = new MainWindowViewModel(GameFolder.FromEnvironment(), window);
            viewModel.CloseRequested += (_, _) => window.Close();
            window.DataContext = viewModel;
            // The controller's buttons as launcher actions (to-do 121); kept alive by its Pad.Pressed handler.
            _ = new PadNavigator(window, viewModel);
            // "--updated": started by the updater (SPEC 9.3), which then closed.
            bool justUpdated = desktop.Args?.Contains("--updated") == true;
            // "--fullscreen": for TVs and handhelds (to-do 143); F11 swaps it back.
            if (desktop.Args?.Contains("--fullscreen") == true)
            {
                window.WindowState = Avalonia.Controls.WindowState.FullScreen;
            }
            window.Opened += async (_, _) =>
            {
                viewModel.Pad.Start();
                // Before the tabs read settings.ini: an older version's may still be in AppData.
                await viewModel.UserFiles.MoveInAsync();
                await viewModel.LoadAsync();
                await Task.WhenAll(viewModel.Files.CheckLatestDecensorAsync(), viewModel.CheckForUpdateAsync(justUpdated));
            };
            // A change made just before closing is still waiting to be written.
            window.Closing += (_, _) => viewModel.Flush();
            // (A launcher that dies closes the helper's input too: it quits by itself.)
            window.Closed += (_, _) => viewModel.Pad.Dispose();
            desktop.MainWindow = window;
        }

        base.OnFrameworkInitializationCompleted();
    }
}
