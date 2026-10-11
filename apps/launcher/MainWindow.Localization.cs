using System.Windows.Controls;

namespace BloodborneLauncher;

public partial class MainWindow
{
    void UiLanguage_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || UiLanguageCombo.SelectedItem is not ComboBoxItem item) return;
        // Commit the current key edit without replacing any unsaved user choices.
        KeyBindingsGrid.CommitEdit();
        KeyBindingsGrid.CommitEdit(DataGridEditingUnit.Row, true);
        state.UiLanguage = Localization.Normalize(item.Tag?.ToString());
        Localization.Current.SetLanguage(state.UiLanguage);
        RefreshBindings();
        RefreshStatusCore(detectGpu: false);
        L10n.SetText(FooterMessage, "Unsaved changes");
    }
}
