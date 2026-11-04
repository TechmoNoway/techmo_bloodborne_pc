// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher;

public partial class MainWindow
{
    bool updatesReady, updatesBusy, updateDownloading, updateHandoff;
    CancellationTokenSource? updateCancellation;
    TechmoRelease? availableUpdate;
    PreparedUpdate? preparedUpdate;
    string UpdatePreferencesPath => Path.Combine(root, "user/launcher/update-settings.json");

    void InitializeUpdates()
    {
        Title = "Techmo Bloodborne · " + TechmoUpdates.CurrentLabel;
        AppVersionFooter.Text = "Techmo " + TechmoUpdates.CurrentLabel + " · Windows x64";
        L10n.SetText(UpdateCurrentVersion, "Installed version: {0}", TechmoUpdates.CurrentLabel);
        bool auto = true;
        try
        {
            if (File.Exists(UpdatePreferencesPath))
            {
                using var preference = JsonDocument.Parse(File.ReadAllText(UpdatePreferencesPath));
                auto = preference.RootElement.GetProperty("automatic").GetBoolean();
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or KeyNotFoundException) { }
        AutoCheckUpdatesCheck.IsChecked = auto;
        updatesReady = true;
        L10n.SetText(UpdateStatus, "Check for new Techmo releases here.");
        try
        {
            string resultPath = Path.Combine(root, ".techmo-update/result.json");
            if (File.Exists(resultPath))
            {
                using var result = JsonDocument.Parse(File.ReadAllText(resultPath));
                L10n.SetText(UpdateStatus, result.RootElement.GetProperty("success").GetBoolean()
                    ? "Update installed: {0}" : "Update failed. Details: {0}",
                    result.RootElement.GetProperty(result.RootElement.GetProperty("success").GetBoolean() ? "version" : "message").GetString() ?? "");
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or KeyNotFoundException) { }
        RefreshUpdateControls();
        Loaded += async (_, _) =>
        {
            // Fixture hosts and source builds never make automatic network requests.
            if (AutoCheckUpdatesCheck.IsChecked == true && TechmoUpdates.CanInstall(root, Environment.ProcessPath))
                await CheckForUpdatesAsync();
        };
        Closed += (_, _) =>
        {
            updateCancellation?.Cancel();
            if (preparedUpdate != null && !updateHandoff)
                try { TechmoUpdates.DeleteStage(root, preparedUpdate.Stage); } catch (IOException) { }
        };
    }

    void RefreshUpdateControls()
    {
        if (!updatesReady) return;
        CheckUpdatesButton.IsEnabled = !updatesBusy && preparedUpdate == null;
        AutoCheckUpdatesCheck.IsEnabled = !updatesBusy;
        InstallUpdateButton.Visibility = availableUpdate != null ? Visibility.Visible : Visibility.Collapsed;
        InstallUpdateButton.IsEnabled = !updatesBusy && !gameRunning && !saveDataBusy && TechmoUpdates.CanInstall(root, Environment.ProcessPath);
        CancelUpdateButton.Visibility = updateDownloading ? Visibility.Visible : Visibility.Collapsed;
        InstallUpdateButton.Content = L10n.Text(preparedUpdate == null ? "Download update" : "Save settings, restart & install");
    }

    async void CheckUpdates_Click(object sender, RoutedEventArgs e) => await CheckForUpdatesAsync();

    async Task CheckForUpdatesAsync()
    {
        if (updatesBusy || preparedUpdate != null) return;
        updatesBusy = true; availableUpdate = null;
        updateCancellation?.Dispose(); updateCancellation = new();
        UpdateReleaseNotes.Text = "";
        L10n.SetText(UpdateStatus, "Checking for updates…"); RefreshUpdateControls();
        try
        {
            using var http = TechmoUpdates.CreateClient();
            var release = await new TechmoUpdates(http).LatestAsync(updateCancellation.Token);
            if (release != null && release.Version > TechmoUpdates.Current)
            {
                availableUpdate = release; UpdateReleaseNotes.Text = release.Notes;
                L10n.SetText(UpdateStatus, TechmoUpdates.CanInstall(root, Environment.ProcessPath)
                    ? "Version {0} is available. Save data and settings are kept."
                    : "Version {0} is available. Install updates from an extracted release package; source checkouts are built separately.", release.Tag);
                SetFooterMessage("Techmo update available: {0}", release.Tag);
            }
            else L10n.SetText(UpdateStatus, "Techmo is up to date ({0}).", TechmoUpdates.CurrentLabel);
        }
        catch (OperationCanceledException) { L10n.SetText(UpdateStatus, "Update check timed out or was cancelled. Try again."); }
        catch (Exception error)
        { L10n.SetText(UpdateStatus, "Could not check for updates: {0}", error.Message); }
        finally { updatesBusy = false; RefreshUpdateControls(); }
    }

    async void InstallUpdate_Click(object sender, RoutedEventArgs e)
    {
        if (updatesBusy || gameRunning || saveDataBusy || availableUpdate == null || !TechmoUpdates.CanInstall(root, Environment.ProcessPath)) return;
        if (preparedUpdate != null)
        {
            if (!SaveSettings()) return;
            try
            {
                TechmoUpdates.StartInstaller(preparedUpdate);
                updateHandoff = true;
                Application.Current.Shutdown();
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            { L10n.SetText(UpdateStatus, "Could not start the update: {0}", error.Message); }
            return;
        }
        updatesBusy = updateDownloading = true;
        updateCancellation?.Dispose(); updateCancellation = new();
        UpdateDownloadProgress.Visibility = Visibility.Visible; UpdateDownloadProgress.Value = 0;
        RefreshUpdateControls();
        try
        {
            using var http = TechmoUpdates.CreateClient();
            var progress = new Progress<UpdateProgress>(value =>
            {
                UpdateDownloadProgress.Value = value.Fraction * 100;
                L10n.SetText(UpdateStatus, value.Phase == "download" ? "Downloading update… {0}%" : "Verifying update… {0}%", (int)(value.Fraction * 100));
            });
            preparedUpdate = await new TechmoUpdates(http).PrepareAsync(availableUpdate, root, progress, updateCancellation.Token);
            L10n.SetText(UpdateStatus, "Update ready. Restart to install; saves, mods and settings are kept.");
        }
        catch (OperationCanceledException) { L10n.SetText(UpdateStatus, "Download cancelled. The installed version was not changed."); }
        catch (Exception error)
        { L10n.SetText(UpdateStatus, "Update download failed: {0}", error.Message); }
        finally
        {
            updatesBusy = updateDownloading = false;
            UpdateDownloadProgress.Visibility = Visibility.Collapsed; RefreshUpdateControls();
        }
    }

    void CancelUpdate_Click(object sender, RoutedEventArgs e) => updateCancellation?.Cancel();

    void AutoCheckUpdates_Changed(object sender, RoutedEventArgs e)
    {
        if (!updatesReady) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(UpdatePreferencesPath)!);
            File.WriteAllText(UpdatePreferencesPath, JsonSerializer.Serialize(new { automatic = AutoCheckUpdatesCheck.IsChecked == true }));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { L10n.SetText(UpdateStatus, "Could not save the update preference: {0}", error.Message); }
    }
}
