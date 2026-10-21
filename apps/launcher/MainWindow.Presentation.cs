using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;

namespace BloodborneLauncher;

public partial class MainWindow
{
    void PresentMode_Changed(object sender, SelectionChangedEventArgs e) => UpdatePresentModeDescription();

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

        AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(SettingsSelection_Changed));
        AddHandler(ToggleButton.CheckedEvent, new RoutedEventHandler(SettingsToggle_Changed));
        AddHandler(ToggleButton.UncheckedEvent, new RoutedEventHandler(SettingsToggle_Changed));
        AddHandler(TextBox.TextChangedEvent, new TextChangedEventHandler((_, _) => MarkSettingsChanged()));
        AddHandler(PasswordBox.PasswordChangedEvent, new RoutedEventHandler((_, _) => MarkSettingsChanged()));
        // ScrollBar also inherits RangeBase: scrolling is navigation, not a setting edit.
        AddHandler(RangeBase.ValueChangedEvent, new RoutedPropertyChangedEventHandler<double>((_, e) =>
        { if (e.OriginalSource is Slider) MarkSettingsChanged(); }));
        PreviewMouseWheel += Settings_MouseWheel;
        SizeChanged += (_, _) => UpdateDeskLayout();
        UpdateDeskLayout();
        StateChanged += (_, _) => MaximizeButton.Content = WindowState == WindowState.Maximized ? "\uE923" : "\uE922";
    }

    void UpdateDeskLayout()
    {
        // Stack the poster above launch controls when two readable columns will not fit.
        double width = ActualWidth > 0 ? ActualWidth : Width;
        bool narrow = width < 900;
        PosterColumn.Width = new GridLength(1, GridUnitType.Star);
        LaunchColumn.Width = narrow ? new GridLength(0) : new GridLength(300);
        Grid.SetColumn(LaunchPanel, narrow ? 0 : 1);
        Grid.SetRow(LaunchPanel, narrow ? 1 : 0);
        GamePoster.MinHeight = narrow ? 290 : 350;
        GamePoster.Height = narrow ? 290 : 350;
        GamePoster.Margin = narrow ? new Thickness(0, 0, 0, 16) : new Thickness(0, 0, 18, 0);
        SidebarColumn.Width = new GridLength(width < 850 ? 150 : 174);
        SidebarArt.Visibility = ActualHeight < 600 ? Visibility.Collapsed : Visibility.Visible;
        Breadcrumb.Visibility = width < 1050 ? Visibility.Collapsed : Visibility.Visible;
        ProfileLabel.Visibility = width < 800 ? Visibility.Collapsed : Visibility.Visible;
        ProfileCombo.Width = width < 800 ? 158 : 200;
        KeyBindingsGrid.Height = Math.Clamp(ActualHeight - 390, 230, 490);
        bool stack = width < 1120;
        GraphicsSideColumn.Width = stack ? new GridLength(0) : new GridLength(238);
        Grid.SetColumn(GraphicsSidebar, stack ? 0 : 1);
        Grid.SetRow(GraphicsSidebar, stack ? 1 : 0);
        GraphicsSidebar.Margin = stack ? new Thickness(0, 0, 0, 16) : new Thickness(16, 0, 0, 0);
        ControlsPreviewColumn.Width = stack ? new GridLength(0) : new GridLength(220);
        Grid.SetColumn(BindingsPanel, stack ? 0 : 1);
        Grid.SetRow(BindingsPanel, stack ? 1 : 0);
        Grid.SetColumnSpan(BindingsPanel, stack ? 2 : 1);
        Grid.SetColumnSpan(ControlsPreview, stack ? 2 : 1);
        ControlsPreview.MaxWidth = stack ? 340 : double.PositiveInfinity;
        ControlsPreview.HorizontalAlignment = stack ? HorizontalAlignment.Left : HorizontalAlignment.Stretch;
        ControlsPreview.Margin = stack ? new Thickness(0) : new Thickness(0, 0, 16, 0);
        ProviderGrid.Columns = width < 800 ? 2 : 3;
        LightingGrid.Columns = width < 900 ? 1 : 2;
        EffectsGrid.Columns = width < 900 ? 1 : 2;
    }

    void ConfigureGraphics_Click(object sender, RoutedEventArgs e) => Nav_Click(NavGraphics, e);

    void DetailedSetting_Changed(object sender, SelectionChangedEventArgs e) => SyncDisplaySummary();
    void DisplaySetting_Changed(object sender, RoutedEventArgs e) => SyncDisplaySummary();

    void UpdateUpscalingModeAvailability()
    {
        string provider = ComboTag(UpscalerCombo);
        bool available = provider is "dlss" or "fsr3" or "fsr4" or "fsr411";
        PresetCombo.IsEnabled = available;
        UpdateSharpnessUi();
        if (!available) PresetCombo.IsDropDownOpen = false;
        // Keep the saved selection: switching back to DLSS/FSR restores it.
        PresetUnavailableHint.Visibility = available ? Visibility.Collapsed : Visibility.Visible;
        L10n.SetText(PresetUnavailableHint, provider switch
        {
            "off" => "Upscaling is off. Quality modes do not apply.",
            "taa" => "TAA uses the output resolution. Quality modes do not apply.",
            _ => "Select the render scale used by DLSS or FSR."
        });
    }

    void SyncDisplaySummary()
    {
        if (syncing) return;
        syncing = true;
        try
        {
            UpdateUpscalingModeAvailability();
            string fps = ComboTag(FpsCombo);
            FooterFps.Text = "   |   " + FpsLabel(fps);
            UpdateHubSummary();
        }
        finally { syncing = false; }
    }

    void SettingsSelection_Changed(object sender, SelectionChangedEventArgs e)
    {
        if(updatingBindings || e.OriginalSource==BindingGroupCombo || e.OriginalSource==ProfileCombo || e.OriginalSource==SaveSetCombo)return;
        if (!syncing && e.OriginalSource is ComboBox) MarkSettingsChanged();
    }

    void SettingsToggle_Changed(object sender, RoutedEventArgs e)
    {
        // Mod rows report edits from their model. Binding/virtualization must not
        // turn a freshly displayed checked row into an unsaved settings change.
        if (e.OriginalSource is CheckBox { DataContext: ModRow }) return;
        if (!syncing && e.OriginalSource is CheckBox) MarkSettingsChanged();
    }
}
