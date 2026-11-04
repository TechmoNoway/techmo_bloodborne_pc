// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher;

public partial class MainWindow
{
    SaveDataStore? saveDataStore;
    bool saveDataBusy;

    void InitializeSaves()
    {
        string user = EngineSavesDirectory;
        saveDataStore = new(Path.GetFullPath(user, root), IsSaveDataInUse);
        SaveRootText.Text = saveDataStore.SaveRoot;
        RefreshSaveLists();
    }

    bool IsSaveDataInUse()
    {
        if (gameRunning) return true;
        var processes = Process.GetProcessesByName("bb-probe");
        try { return processes.Length > 0; }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    void RefreshSaveLists()
    {
        if (saveDataStore is null || saveDataBusy) return;
        try
        {
            string? user = (SaveSetCombo.SelectedItem as SaveDataSet)?.UserId;
            string? title = (SaveSetCombo.SelectedItem as SaveDataSet)?.TitleId;
            string? backup = (SaveBackupList.SelectedItem as SaveBackup)?.Path;
            var saves = saveDataStore.ListSaves();
            var backups = saveDataStore.ListBackups();
            // Equal record values can keep WPF's cached selection text after a language change.
            SaveSetCombo.ItemsSource = null;
            SaveSetCombo.ItemsSource = saves;
            SaveSetCombo.SelectedItem = saves.FirstOrDefault(s => s.UserId == user && s.TitleId == title) ?? saves.FirstOrDefault();
            SaveBackupList.ItemsSource = backups;
            SaveBackupList.SelectedItem = backups.FirstOrDefault(b => b.Path == backup) ?? backups.FirstOrDefault();
            L10n.SetText(SaveDataStatus, IsSaveDataInUse() ? "Close the game before backing up or restoring save data." :
                saves.Count == 0 ? "No save data found in this launcher’s user folder." : "{0} save set(s) · {1} backup(s)", saves.Count, backups.Count);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        { L10n.SetText(SaveDataStatus, "Could not read save data: {0}", error.Message); }
        UpdateSaveActions();
    }

    void UpdateSaveActions()
    {
        if (BackupSaveButton is null) return;
        bool available = !saveDataBusy && !IsSaveDataInUse();
        BackupSaveButton.IsEnabled = available && SaveSetCombo.SelectedItem is SaveDataSet;
        RestoreSaveButton.IsEnabled = available && SaveBackupList.SelectedItem is SaveBackup;
        ImportSaveButton.IsEnabled = available;
        ExportSaveButton.IsEnabled = !saveDataBusy && SaveBackupList.SelectedItem is SaveBackup;
        SaveRefreshButton.IsEnabled = SaveSetCombo.IsEnabled = SaveBackupList.IsEnabled = !saveDataBusy;
        SaveOperationProgress.Visibility = saveDataBusy ? Visibility.Visible : Visibility.Collapsed;
    }

    async Task RunSaveOperation(Func<string> operation)
    {
        if (saveDataBusy) return;
        saveDataBusy = true; UpdateSaveActions(); RefreshStatusCore(detectGpu: false);
        L10n.SetText(SaveDataStatus, "Working with save data…");
        string? result = null; Exception? failure = null;
        try { result = await Task.Run(operation); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or System.Text.Json.JsonException)
        { failure = error; }
        finally { saveDataBusy = false; RefreshSaveLists(); RefreshStatusCore(detectGpu: false); }
        if (failure is not null) L10n.SetText(SaveDataStatus, "Save operation failed: {0}", L10n.Text(failure.Message));
        else if (result is not null) L10n.SetText(SaveDataStatus, result);
    }

    void SaveSelection_Changed(object sender, SelectionChangedEventArgs e) => UpdateSaveActions();
    void RefreshSaves_Click(object sender, RoutedEventArgs e) => RefreshSaveLists();

    async void BackupSave_Click(object sender, RoutedEventArgs e)
    {
        if (SaveSetCombo.SelectedItem is not SaveDataSet set || saveDataStore is null) return;
        await RunSaveOperation(() => { var backup = saveDataStore.Backup(set.UserId, set.TitleId); return L10n.Text("Backup created: {0}", Path.GetFileName(backup.Path)); });
    }
    async void RestoreSave_Click(object sender, RoutedEventArgs e)
    { if (SaveBackupList.SelectedItem is SaveBackup backup) await ConfirmAndRestoreSave(backup); }

    async Task ConfirmAndRestoreSave(SaveBackup backup)
    {
        if (saveDataStore is null || saveDataBusy) return;
        if (MessageBox.Show(this, L10n.Text("Restore {0} for user {1}? This replaces that save set. A safety backup of the current data is created first.", backup.TitleId, backup.UserId),
            L10n.Text("Restore save data"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        await RunSaveOperation(() =>
        {
            string? safety = saveDataStore.Restore(backup.Path);
            return safety is null ? L10n.Text("Save data restored.") : L10n.Text("Save data restored. Previous data: {0}", Path.GetFileName(safety));
        });
    }

    async void ImportSave_Click(object sender, RoutedEventArgs e)
    {
        if (saveDataStore is null) return;
        var dialog = new OpenFileDialog { Title = L10n.Text("Import Techmo save backup"), Filter = "Techmo save backup (*.zip)|*.zip", CheckFileExists = true };
        if (dialog.ShowDialog(this) != true) return;
        try { await ConfirmAndRestoreSave(saveDataStore.Inspect(dialog.FileName)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or System.Text.Json.JsonException)
        { L10n.SetText(SaveDataStatus, "Save operation failed: {0}", error.Message); }
    }

    async void ExportSave_Click(object sender, RoutedEventArgs e)
    {
        if (SaveBackupList.SelectedItem is not SaveBackup backup) return;
        var dialog = new SaveFileDialog { Title = L10n.Text("Export save backup"), Filter = "Techmo save backup (*.zip)|*.zip", FileName = Path.GetFileName(backup.Path), OverwritePrompt = true };
        if (dialog.ShowDialog(this) != true) return;
        if (Path.GetFullPath(dialog.FileName).Equals(backup.Path, StringComparison.OrdinalIgnoreCase)) return;
        await RunSaveOperation(() => { File.Copy(backup.Path, dialog.FileName, overwrite: true); return L10n.Text("Backup exported."); });
    }

    void OpenSaveFolder_Click(object sender, RoutedEventArgs e)
    {
        if (saveDataStore is null) return;
        string path = sender is Button { CommandParameter: "backups" } ? saveDataStore.BackupRoot : saveDataStore.SaveRoot;
        try
        {
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo("explorer.exe", path) { UseShellExecute = true });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        { L10n.SetText(SaveDataStatus, "Save operation failed: {0}", error.Message); }
    }

    void SavesTab_Click(object sender, RoutedEventArgs e)
    {
        bool saves = sender is not Button { CommandParameter: "Mods" };
        if (!saves) _ = RefreshModsAsync();
        SavesTab.Tag = saves ? "active" : null; ModsTab.Tag = saves ? null : "active";
        SaveDataPanel.Visibility = saves ? Visibility.Visible : Visibility.Collapsed;
        ModSettingsPanel.Visibility = saves ? Visibility.Collapsed : Visibility.Visible;
        if (saves) RefreshSaveLists();
        AnimateHub(saves ? SaveDataPanel : ModSettingsPanel);
    }
}
