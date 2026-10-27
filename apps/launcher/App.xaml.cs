using System.Windows;

namespace BloodborneLauncher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // An integration host can supply its own shell before the dispatcher starts.
        if (MainWindow is null) { MainWindow = new MainWindow(); MainWindow.Show(); }
    }
}
