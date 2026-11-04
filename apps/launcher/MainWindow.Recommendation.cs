// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using N = Techmo.Engine.Core;

namespace BloodborneLauncher;

public partial class MainWindow
{
    sealed record GraphicsRecommendation(N.Hardware Hardware, N.LauncherSettings Settings, N.IniFile Ini,
        IReadOnlyList<N.Hardware.Change> Changes);

    GraphicsRecommendation CreateGraphicsRecommendation(N.Hardware hardware)
    {
        // Recommend against a disposable in-memory copy, including pending Techmo edits.
        // Its Apply closures never touch live state or disk while the preview is open.
        var ini = new N.IniFile();
        foreach (var pair in N.Workspace.Current.Ini.Values) ini.Set(pair.Key, pair.Value);
        ini.Set("upscaler", ComboTag(UpscalerCombo, "fsr3"));
        ini.Set("preset", ComboTag(PresetCombo, "1"));
        ini.Set("output_res", ComboText(ResolutionCombo, "1920x1080"));
        var settings = new N.LauncherSettings();
        settings.Set("fps_mode", ComboTag(FpsCombo, "60"));
        return new(hardware, settings, ini, hardware.Recommend(settings, ini));
    }

    void StageGraphicsRecommendation(GraphicsRecommendation recommendation)
    {
        if (recommendation.Changes.Count == 0) return;
        foreach (var change in recommendation.Changes) change.Apply();
        var ini = recommendation.Ini;
        syncing = true;
        try
        {
            SetComboTag(UpscalerCombo, ini.Get("upscaler"));
            SetComboTag(PresetCombo, ini.Get("preset"));
            SetComboContent(ResolutionCombo, ini.Get("output_res"));
            SetComboTag(FpsCombo, recommendation.Settings.Str("fps_mode"));
            N.Workspace.Current.Ini.Set("object_motion", ini.Get("object_motion"));
            var motion = engineOptionSources["GraphicsPage"].HostedRows.Single(row => row.Title == "Object motion vectors");
            ((Wpf.Ui.Controls.ToggleSwitch)motion.Editor!).IsChecked = ini.Flag("object_motion");
        }
        finally { syncing = false; }
        SyncDisplaySummary();
        MarkSettingsChanged();
        L10n.SetText(FooterMessage, "Recommendation selected. Use Apply to save or Undo to discard.");
    }

    void RecommendGraphics_Click(object sender, RoutedEventArgs e)
    {
        if (!engineOptionsReady) return;
        var recommendation = CreateGraphicsRecommendation(N.Hardware.Detect());
        if (BuildGraphicsRecommendationDialog(recommendation).ShowDialog() == true)
            StageGraphicsRecommendation(recommendation);
    }

    Window BuildGraphicsRecommendationDialog(GraphicsRecommendation recommendation)
    {
        var dialog = new Window { Owner = this, Title = L10n.Text("Recommended graphics"), Width = 640,
            MaxHeight = Math.Max(300, SystemParameters.WorkArea.Height - 40), SizeToContent = SizeToContent.Height,
            ResizeMode = ResizeMode.NoResize, WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Background = (Brush)FindResource("PopoverBrush"), Foreground = (Brush)FindResource("TextBrush"),
            FontFamily = FontFamily, FontSize = 13, ShowInTaskbar = false };
        var panel = new StackPanel { Margin = new Thickness(24) };
        panel.Children.Add(new TextBlock { Text = L10n.Text("Recommended graphics"), Style = (Style)FindResource("PageTitle") });
        panel.Children.Add(new TextBlock { Text = recommendation.Hardware.Summary, TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 4, 0, 16), Foreground = (Brush)FindResource("MutedBrush") });
        var table = new Grid { Margin = new Thickness(0, 4, 0, 16) };
        table.ColumnDefinitions.Add(new() { Width = new GridLength(1.2, GridUnitType.Star) });
        table.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        table.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        void Row(string label, string from, string to, bool header = false)
        {
            int index = table.RowDefinitions.Count;
            table.RowDefinitions.Add(new() { Height = GridLength.Auto });
            int column = 0;
            foreach (var value in new[] { label, from, to })
            {
                var cell = new TextBlock { Text = L10n.Text(value), TextWrapping = TextWrapping.Wrap,
                    Margin = new Thickness(0, 6, 12, 6), FontWeight = header ? FontWeights.SemiBold : FontWeights.Normal };
                Grid.SetRow(cell, index); Grid.SetColumn(cell, column++); table.Children.Add(cell);
            }
        }
        if (recommendation.Changes.Count > 0)
        {
            Row("Setting", "Current", "Recommended", header: true);
            foreach (var change in recommendation.Changes) Row(change.Label, change.From, change.To);
            panel.Children.Add(table);
        }
        panel.Children.Add(new TextBlock { Text = L10n.Text(recommendation.Changes.Count == 0
                ? "Your current settings already match the recommendation for this PC."
                : "Based on your hardware, not a benchmark. Only the listed settings will change. Use Apply in the launcher to save; Undo discards them."),
            TextWrapping = TextWrapping.Wrap, Foreground = (Brush)FindResource("MutedBrush") });
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 20, 0, 0) };
        var cancel = new Button { Content = L10n.Text(recommendation.Changes.Count > 0 ? "Cancel" : "Close"),
            IsCancel = true, Margin = new Thickness(0, 0, 8, 0) };
        actions.Children.Add(cancel);
        if (recommendation.Changes.Count > 0)
        {
            var use = new Button { Content = L10n.Text("Use recommendation"), IsDefault = true };
            use.Click += (_, _) => dialog.DialogResult = true;
            actions.Children.Add(use);
        }
        panel.Children.Add(actions);
        dialog.Content = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        return dialog;
    }
}
