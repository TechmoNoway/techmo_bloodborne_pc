// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Security;
using System.Text.Json;
using System.Windows;

namespace BloodborneLauncher;

public partial class MainWindow
{
    sealed record AppliedSettings(LauncherState Settings, Dictionary<string, string> Bindings,
        string? ProfileId, SecureString Password, SecureString Token, ModSelection Mods) : IDisposable
    {
        public void Dispose() { Password.Dispose(); Token.Dispose(); }
    }

    AppliedSettings? appliedSettings;

    static LauncherState CloneSettings(LauncherState value) =>
        JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(value))!;

    void InitializeUndo()
    {
        RememberAppliedSettings();
        Closed += (_, _) => { appliedSettings?.Dispose(); appliedSettings = null; };
    }

    void RememberAppliedSettings()
    {
        CollectUi();
        var next = new AppliedSettings(CloneSettings(state), keyBindings.ToDictionary(row => row.Key, row => row.Binding),
            activeProfile?.Id, OnlinePasswordBox.SecurePassword, OnlineTokenBox.SecurePassword, modSelection.Copy());
        appliedSettings?.Dispose();
        appliedSettings = next;
        UndoSettingsButton.IsEnabled = hasUnsavedChanges;
    }

    void UndoSettings_Click(object sender, RoutedEventArgs e)
    {
        if (!hasUnsavedChanges || appliedSettings is not {} saved) return;
        EndInputCapture(restoreFocus: false);
        syncing = true;
        try
        {
            state = CloneSettings(saved.Settings);
            modSelection = saved.Mods.Copy();
            modSelectionEdited = false;
            UpdateModCatalog(modCatalog);
            foreach (var row in keyBindings) row.Binding = saved.Bindings[row.Key];
            // Profile rename/delete operations are immediate and are not undone here.
            activeProfile = profileLibrary.Profiles.FirstOrDefault(profile => profile.Id == saved.ProfileId) ?? activeProfile;
            ApplyStateToUi();
            ReloadLanternOptions();
            syncing = true;
            OnlinePasswordBox.Password = new NetworkCredential("", saved.Password).Password;
            OnlineTokenBox.Password = new NetworkCredential("", saved.Token).Password;
            hasUnsavedChanges = false;
            if (!profilesUnavailable) RefreshProfilePicker();
        }
        finally { syncing = false; }
        SyncDisplaySummary();
        RefreshStatusCore(detectGpu: false);
        LoadHeroImage();
        UpdateSaveState();
        L10n.SetText(FooterMessage, "Unapplied changes undone");
    }
}
