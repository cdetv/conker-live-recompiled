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
            // "--updated": started by the updater (SPEC 9.3), which then closed.
            bool justUpdated = desktop.Args?.Contains("--updated") == true;
            window.Opened += async (_, _) =>
            {
                await viewModel.LoadAsync();
                await Task.WhenAll(viewModel.Files.CheckLatestDecensorAsync(), viewModel.CheckForUpdateAsync(justUpdated));
            };
            // A change made just before closing is still waiting to be written.
            window.Closing += (_, _) => viewModel.Flush();
            desktop.MainWindow = window;
        }

        base.OnFrameworkInitializationCompleted();
    }
}
