using System.Windows;

namespace BloodborneLauncher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // An integration host can supply its own shell before the dispatcher starts.
        if (MainWindow is null)
        {
            for (var folder = new System.IO.DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            {
                if (!System.IO.File.Exists(System.IO.Path.Combine(folder.FullName, "engine.lock.json"))) continue;
                if (TechmoUpdates.InstallInProgress(folder.FullName))
                {
                    MessageBox.Show("Techmo is installing an update. It will reopen when the installation finishes.", "Techmo Bloodborne");
                    Shutdown(); return;
                }
                break;
            }
            MainWindow = new MainWindow(); MainWindow.Show();
        }
    }
}
