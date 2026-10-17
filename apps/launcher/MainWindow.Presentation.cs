using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;

namespace BloodborneLauncher;

public partial class MainWindow
{
    bool syncingVSync;
    string lastNonVSyncMode = "";

    void SyncVSyncChoice()
    {
        if (syncingVSync) return;
        syncingVSync = true;
        try
        {
            string mode = ComboTag(PresentCombo);
            bool enabled = mode is "Fifo" or "FifoRelaxed";
            if (!enabled) lastNonVSyncMode = mode;
            VSyncCheck.IsChecked = enabled;
        }
        finally { syncingVSync = false; }
    }

    void PresentMode_Changed(object sender, SelectionChangedEventArgs e) => SyncVSyncChoice();

    void VSyncChoice_Changed(object sender, RoutedEventArgs e)
    {
        if (syncingVSync || syncing) return;
        syncingVSync = true;
        try
        {
            SetComboTag(PresentCombo, VSyncCheck.IsChecked == true ? "Fifo" : lastNonVSyncMode);
        }
        finally { syncingVSync = false; }
    }

    // Keep presentation events separate from configuration and launch contracts.
    void InitializePresentation()
    {
        InitializeInputCapture();
        var workArea = SystemParameters.WorkArea;
        MinWidth = Math.Min(MinWidth, Math.Max(640, workArea.Width - 24));
        MinHeight = Math.Min(MinHeight, Math.Max(480, workArea.Height - 24));
        Width = Math.Min(Width, workArea.Width - 24);
        Height = Math.Min(Height, workArea.Height - 24);

        ResolutionCombo.SelectionChanged += DetailedSetting_Changed;
        UpscalerCombo.SelectionChanged += DetailedSetting_Changed;
        PresetCombo.SelectionChanged += DetailedSetting_Changed;
        FpsCombo.SelectionChanged += DetailedSetting_Changed;
        FullscreenCheck.Checked += DisplaySetting_Changed;
        FullscreenCheck.Unchecked += DisplaySetting_Changed;
        PresentCombo.SelectionChanged += PresentMode_Changed;
        VSyncCheck.Checked += VSyncChoice_Changed;
        VSyncCheck.Unchecked += VSyncChoice_Changed;

        AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(SettingsSelection_Changed));
        AddHandler(ToggleButton.CheckedEvent, new RoutedEventHandler(SettingsToggle_Changed));
        AddHandler(ToggleButton.UncheckedEvent, new RoutedEventHandler(SettingsToggle_Changed));
        SizeChanged += (_, _) => UpdateDeskLayout();
        UpdateDeskLayout();
        StateChanged += (_, _) => MaximizeButton.Content = WindowState == WindowState.Maximized ? "\uE923" : "\uE922";
    }

    void UpdateDeskLayout()
    {
        // Stack the poster above launch controls when two readable columns will not fit.
        bool narrow = ActualWidth < 1050;
        PosterColumn.Width = new GridLength(1, GridUnitType.Star);
        LaunchColumn.Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        Grid.SetColumn(LaunchPanel, narrow ? 0 : 1);
        Grid.SetRow(LaunchPanel, narrow ? 1 : 0);
        GamePoster.MinHeight = narrow ? 260 : 480;
        GamePoster.Height = narrow ? 260 : double.NaN;
        GamePoster.Margin = narrow ? new Thickness(0, 0, 0, 20) : new Thickness(0, 0, 24, 0);
        QuickSettingsGrid.Columns = ActualWidth < 850 ? 2 : ActualWidth < 1250 ? 3 : 5;
        SidebarColumn.Width = new GridLength(ActualWidth < 850 ? 160 : 188);
        SidebarArt.Height = ActualHeight < 800 ? 54 : 100;
        LanguageLabel.Visibility = ActualWidth < 850 ? Visibility.Collapsed : Visibility.Visible;
        KeyBindingsGrid.Height = Math.Clamp(ActualHeight - 470, 190, 440);
    }

    void ConfigureGraphics_Click(object sender, RoutedEventArgs e) => Nav_Click(NavGraphics, e);

    void DetailedSetting_Changed(object sender, SelectionChangedEventArgs e) => SyncQuickSettings();
    void DisplaySetting_Changed(object sender, RoutedEventArgs e) => SyncQuickSettings();

    void UpdateUpscalingModeAvailability()
    {
        string provider = ComboTag(UpscalerCombo);
        bool available = provider is "dlss" or "fsr3" or "fsr4" or "fsr411";
        PresetCombo.IsEnabled = QuickPreset.IsEnabled = available;
        if (!available) PresetCombo.IsDropDownOpen = QuickPreset.IsDropDownOpen = false;
        // Keep the saved selection: switching back to DLSS/FSR restores it.
        PresetUnavailableHint.Visibility = available ? Visibility.Collapsed : Visibility.Visible;
        L10n.SetText(PresetUnavailableHint, provider switch
        {
            "off" => "Upscaling is off. Quality modes do not apply.",
            "taa" => "TAA uses the output resolution. Quality modes do not apply.",
            _ => "Select the render scale used by DLSS or FSR."
        });
    }

    void SyncQuickSettings()
    {
        if (syncing) return;
        syncing = true;
        try
        {
            SetComboContent(QuickResolution, ComboText(ResolutionCombo));
            QuickDisplay.SelectedIndex = FullscreenCheck.IsChecked == true ? 0 : 1;
            SetQuickUpscaler(ComboTag(UpscalerCombo));
            QuickPreset.SelectedIndex = PresetCombo.SelectedIndex;
            UpdateUpscalingModeAvailability();
            string fps = ComboTag(FpsCombo);
            SetQuickFps(fps);
            FooterFps.Text = "   |   " + FpsLabel(fps);
        }
        finally { syncing = false; }
    }

    void SettingsSelection_Changed(object sender, SelectionChangedEventArgs e)
    {
        if(updatingBindings || e.OriginalSource==BindingGroupCombo)return;
        if (!syncing && e.OriginalSource is ComboBox) L10n.SetText(FooterMessage, "Unsaved changes");
    }

    void SettingsToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (!syncing && e.OriginalSource is CheckBox) L10n.SetText(FooterMessage, "Unsaved changes");
    }
}
