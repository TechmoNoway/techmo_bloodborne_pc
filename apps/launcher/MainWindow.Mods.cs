// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher;

public partial class MainWindow
{
    public sealed class ModRow : INotifyPropertyChanged
    {
        internal ModInfo Info { get; init; } = new();
        public string Name => Info.Name;
        public string Path => Info.Path;
        public bool CanToggle => Info.CanToggle;
        public bool CanMove => Info.Kind == "named" && Info.Error == "";
        public string Priority { get; internal set; } = "";
        internal int Conflicts;
        bool enabled;
        public bool Enabled
        {
            get => enabled;
            set { if (enabled == value) return; enabled = value; Changed?.Invoke(this); Refresh(); }
        }
        public string Status => Info.Error switch
        {
            "layout" => L10n.Text("Folder layout not recognized"), "empty" => L10n.Text("No game files found"),
            "archive" => L10n.Text("Extract this archive into a mod folder first"),
            "type" => L10n.Text("This file type is not a loose-file mod"),
            "invalid" => L10n.Text("Cannot load: {0}", Info.Detail),
            _ => L10n.Text("{0} files · {1} shared", Info.Files.Length, Conflicts)
        };
        public string Source => L10n.Text(Info.Kind switch
        { "legacy" => "Game's sibling mod folder", "direct" => "Loose files at the mods root", _ => "Folder mod" });
        internal event Action<ModRow>? Changed;
        public event PropertyChangedEventHandler? PropertyChanged;
        internal void Refresh() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(""));
    }

    public sealed record ModConflict(string File, string Owners, string Winner)
    {
        public string Description => L10n.Text("{0} · Winner: {1}", Owners, Winner);
    }

    readonly ObservableCollection<ModRow> modRows = new();
    ModSelection modSelection = new();
    string modsDirectory = "", modsConfigPath = "";
    string? modConfigError;
    bool modsReady, modsBusy, modsRescanRequested, updatingModRows, modSelectionEdited;
    ModCatalog modCatalog = new();

    void LoadModSettings()
    {
        modsDirectory = Path.GetFullPath(Environment.GetEnvironmentVariable("BB_MODS_DIR") ?? Path.Combine(root, "mods"), root);
        modsConfigPath = Path.GetFullPath(Environment.GetEnvironmentVariable("BB_MODS_CONFIG") ?? Path.Combine(root, "mods.json"), root);
        try { modSelection = ModSelection.Read(modsConfigPath); modConfigError = null; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        { modSelection = new(); modConfigError = error.Message; }
        modSelectionEdited = false;
        if (modsReady) UpdateModCatalog(modCatalog);
    }

    void InitializeMods()
    {
        modsReady = true;
        ModsFolderText.Text = modsDirectory;
        ModList.ItemsSource = modRows;
        _ = RefreshModsAsync();
    }

    async Task RefreshModsAsync()
    {
        if (!modsReady) return;
        if (modsBusy) { modsRescanRequested = true; return; }
        modsBusy = true; UpdateModActions();
        L10n.SetText(ModScanStatus, "Reading mod folders…");
        try
        {
            var catalog = await ModCatalog.Scan(root, modsDirectory, PathBox.Text.Trim());
            UpdateModCatalog(catalog);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or OperationCanceledException or System.ComponentModel.Win32Exception)
        { L10n.SetText(ModScanStatus, "Could not read mods: {0}", error.Message); }
        finally
        {
            modsBusy = false; UpdateModActions();
            if (modsRescanRequested) { modsRescanRequested = false; _ = RefreshModsAsync(); }
        }
    }

    void UpdateModCatalog(ModCatalog catalog)
    {
        modCatalog = catalog;
        string? selectedId = (ModList.SelectedItem as ModRow)?.Info.Id;
        updatingModRows = true;
        try
        {
            modRows.Clear();
            int Rank(ModInfo info) => info.Kind switch
            {
                "legacy" => -2, "direct" => -1,
                _ => modSelection.Order.IndexOf(info.Name) is int index && index >= 0 ? index : int.MaxValue
            };
            foreach (var info in catalog.Mods.OrderBy(Rank))
            {
                var row = new ModRow { Info = info, Enabled = info.CanToggle && modSelection.Enabled(info) };
                row.Changed += ModRowChanged;
                modRows.Add(row);
            }
            ModList.SelectedItem = modRows.FirstOrDefault(row => row.Info.Id == selectedId) ?? modRows.FirstOrDefault();
        }
        finally { updatingModRows = false; }
        UpdateModConflicts(); UpdateModActions();
        L10n.SetText(ModScanStatus, modRows.Count == 0 ? "No mods found. Extract each mod into its own folder, then Refresh." :
            "Folder scan complete. File lists do not verify gameplay compatibility.");
    }

    void ModRowChanged(ModRow row)
    {
        if (updatingModRows || syncing) return;
        CaptureModSelection(); UpdateModConflicts(); MarkSettingsChanged();
    }

    void CaptureModSelection()
    {
        var named = modRows.Where(row => row.Info.Kind == "named" && row.CanToggle).ToArray();
        var names = named.Select(row => row.Name).ToHashSet(StringComparer.Ordinal);
        modSelection.Order = named.Select(row => row.Name).Concat(modSelection.Order.Where(name => !names.Contains(name))).Distinct(StringComparer.Ordinal).ToList();
        modSelection.Disabled = named.Where(row => !row.Enabled).Select(row => row.Name)
            .Concat(modSelection.Disabled.Where(name => !names.Contains(name))).Distinct(StringComparer.Ordinal).ToList();
        foreach (var row in modRows)
            if (row.CanToggle && row.Info.Kind == "legacy") modSelection.LegacyEnabled = row.Enabled;
            else if (row.CanToggle && row.Info.Kind == "direct") modSelection.DirectEnabled = row.Enabled;
        modSelectionEdited = true;
    }

    void UpdateModConflicts()
    {
        if (!modsReady) return;
        var owners = new Dictionary<string, List<ModRow>>(StringComparer.OrdinalIgnoreCase);
        int priority = 0;
        foreach (var row in modRows)
        {
            row.Conflicts = 0; row.Priority = row.CanToggle ? (++priority).ToString() : "—";
            if (ModsEnabledCheck.IsChecked != true || !row.Enabled || row.Info.Error != "") continue;
            foreach (string path in row.Info.Files.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (!owners.TryGetValue(path, out var users)) owners[path] = users = new();
                users.Add(row);
            }
        }
        var conflicts = owners.Where(pair => pair.Value.Count > 1).ToArray();
        foreach (var pair in conflicts) foreach (var row in pair.Value) row.Conflicts++;
        foreach (var row in modRows) row.Refresh();
        ModConflictList.ItemsSource = conflicts.Take(200).Select(pair => new ModConflict(pair.Key,
            string.Join(" → ", pair.Value.Select(row => row.Name)), pair.Value[^1].Name)).ToList();
        ModConflictList.Visibility = conflicts.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        L10n.SetText(ModConflictSummary, conflicts.Length == 0 ? "No shared files among enabled mods." :
            "{0} shared file(s). The lower mod wins; the list shows up to 200 files.", conflicts.Length);
        L10n.SetText(ModSummary, ModsEnabledCheck.IsChecked == true ? "Next launch: {0} enabled mod(s)." : "Mods are disabled for the next launch.",
            modRows.Count(row => row.Enabled && row.Info.Error == ""));
        ModConfigWarning.Visibility = modConfigError is null ? Visibility.Collapsed : Visibility.Visible;
        if (modConfigError is not null) L10n.SetText(ModConfigWarning, "Mod configuration could not be read and was preserved: {0}", modConfigError);
        ModList.IsEnabled = modConfigError is null && !modsBusy;
    }

    void UpdateModActions()
    {
        if (!modsReady) return;
        var selected = ModList.SelectedItem as ModRow;
        var movable = modRows.Where(row => row.CanMove).ToList();
        int index = selected is null ? -1 : movable.IndexOf(selected);
        ModMoveUpButton.IsEnabled = !modsBusy && modConfigError is null && index > 0;
        ModMoveDownButton.IsEnabled = !modsBusy && modConfigError is null && index >= 0 && index < movable.Count - 1;
        ModOpenButton.IsEnabled = !modsBusy && selected is not null && selected.Info.Error == "";
        ModRefreshButton.IsEnabled = !modsBusy;
        ModList.IsEnabled = !modsBusy && modConfigError is null;
        ModScanProgress.Visibility = modsBusy ? Visibility.Visible : Visibility.Collapsed;
        SelectedModPath.Text = selected?.Path ?? "";
    }

    void ValidateModSettings()
    {
        if (modConfigError is not null)
        {
            if (state.ModsEnabled) throw new InvalidDataException(L10n.Text("Fix mods.json or turn off mods before applying."));
            return;
        }
        if (state.ModsEnabled && modRows.Any(row => row.Enabled && row.Info.Error != ""))
            throw new InvalidDataException(L10n.Text("Disable invalid mods before applying."));
    }
    void SaveModSettings()
    { if (modConfigError is null && modSelectionEdited) { modSelection.Save(modsConfigPath); modSelectionEdited = false; } }

    void ModsMaster_Changed(object sender, RoutedEventArgs e) => UpdateModConflicts();
    void ModSelection_Changed(object sender, SelectionChangedEventArgs e) => UpdateModActions();
    async void RefreshMods_Click(object sender, RoutedEventArgs e)
    {
        if (modConfigError is not null && !modSelectionEdited) LoadModSettings();
        await RefreshModsAsync();
    }
    void MoveMod_Click(object sender, RoutedEventArgs e)
    {
        if (modsBusy || modConfigError is not null || ModList.SelectedItem is not ModRow { CanMove: true } row) return;
        var movable = modRows.Where(item => item.CanMove).ToList();
        int index = movable.IndexOf(row), target = index + (sender == ModMoveUpButton ? -1 : 1);
        if (target < 0 || target >= movable.Count) return;
        modRows.Move(modRows.IndexOf(row), modRows.IndexOf(movable[target]));
        CaptureModSelection(); UpdateModConflicts(); UpdateModActions(); MarkSettingsChanged();
    }
    void OpenSelectedMod_Click(object sender, RoutedEventArgs e)
    {
        if (ModList.SelectedItem is not ModRow { Info.Error: "" } row) return;
        try { Process.Start(new ProcessStartInfo(row.Path) { UseShellExecute = true }); }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        { L10n.SetText(ModScanStatus, "Could not open mod folder: {0}", error.Message); }
    }
    void RefreshModLanguage()
    { UpdateModConflicts(); UpdateModActions(); }
}
