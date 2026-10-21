// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.Win32;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace BloodborneLauncher;

public partial class MainWindow
{
    bool hubReady;
    string currentHubPage = "Home";

    void InitializeHub()
    {
        hubReady = true;
        HomeSessionCard.Visibility = ShowLastSessionCard ? Visibility.Visible : Visibility.Collapsed;
        GamePoster.SizeChanged += (_, _) => GamePoster.Clip = new RectangleGeometry(new Rect(0, 0, GamePoster.ActualWidth, GamePoster.ActualHeight), 14, 14);
        ApplyAppearance();
        UpdatePresentModeDescription();
        UpdateHubSummary();
        CommandBindings.Add(new CommandBinding(ApplicationCommands.Save, (_, _) => SaveSettings()));
        CommandBindings.Add(new CommandBinding(ApplicationCommands.SaveAs, (_, _) => SaveProfileCopy_Click(this, new RoutedEventArgs()),
            (_, e) => e.CanExecute = !profilesUnavailable && profileLibrary.Profiles.Count < LauncherProfileStore.MaxProfiles));
        InputBindings.Add(new KeyBinding(ApplicationCommands.Save, new KeyGesture(Key.S, ModifierKeys.Control)));
        InputBindings.Add(new KeyBinding(ApplicationCommands.SaveAs, new KeyGesture(Key.S, ModifierKeys.Control | ModifierKeys.Shift)));
        Localization.Current.PropertyChanged += HubLanguageChanged;
        SystemEvents.UserPreferenceChanged += SystemAppearanceChanged;
        Closing += (_, e) =>
        {
            if (saveDataBusy) { e.Cancel = true; SetFooterMessage("Wait for the save operation to finish."); return; }
            if (hasUnsavedChanges && !ConfirmProfileSwitch("Save your changes before closing the launcher?")) e.Cancel = true;
        };
        Closed += (_, _) =>
        {
            Localization.Current.PropertyChanged -= HubLanguageChanged;
            SystemEvents.UserPreferenceChanged -= SystemAppearanceChanged;
        };
    }

    void HubLanguageChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        UpdateHubSummary(); UpdatePresentModeDescription(); UpdateKeycapLabels(); UpdateBreadcrumb(); RefreshSaveLists(); RefreshModLanguage();
    }

    void NavigateHub(string page)
    {
        var pages = new (string id, Button button, FrameworkElement view)[]
        {
            ("Home", NavHome, HomePage), ("Display", NavDisplay, DisplayPage),
            ("Graphics", NavGraphics, GraphicsPage), ("Controller", NavController, ControllerPage),
            ("Mods", NavMods, ModsPage), ("Online", NavOnline, OnlinePage),
            ("Advanced", NavAdvanced, AdvancedPage), ("General", NavGeneral, GeneralPage)
        }.Concat(lanternNavigation).ToArray();
        if (!pages.Any(p => p.id == page)) return;
        currentHubPage = page;
        if (page == "Mods") { RefreshSaveLists(); _ = RefreshModsAsync(); }
        foreach (var entry in pages)
        {
            bool selected = entry.id == page;
            entry.button.Tag = selected ? "active" : null;
            entry.view.Visibility = selected ? Visibility.Visible : Visibility.Collapsed;
            if (selected)
            {
                if (entry.view is ScrollViewer scroll) scroll.ScrollToTop();
                AnimateHub(entry.view);
            }
        }
        UpdateBreadcrumb();
        if (lanternPages.TryGetValue(page, out var nativePage)) nativePage.OnShown();
    }

    void UpdateBreadcrumb() => PageBreadcrumb.Text = Localization.Current[currentHubPage switch
    {
        "Display" => "Ui.Display", "Graphics" => "Ui.Graphics", "Controller" => "Ui.Controls",
        "Mods" => "Saves.Title", "Online" => "Ui.Online", "Advanced" => "Hub.Diagnostics",
        "General" => "Hub.Settings", "Trophies" => "Ui.Trophies", "Engine" => "Ui.Engine_switches", _ => "Ui.Play"
    }];

    void Shortcut_Click(object sender, RoutedEventArgs e)
    { if (sender is Button button) NavigateHub(button.CommandParameter?.ToString() ?? "Home"); }

    bool MotionEnabled => hubReady && IsLoaded && !state.ReducedMotion && SystemParameters.ClientAreaAnimation;

    void AnimateHub(FrameworkElement view)
    {
        view.BeginAnimation(OpacityProperty, null);
        view.Opacity = 1;
        view.RenderTransform = Transform.Identity;
        if (!MotionEnabled) return;
        var easing = new CubicEase { EasingMode = EasingMode.EaseOut };
        var duration = TimeSpan.FromMilliseconds(240);
        view.BeginAnimation(OpacityProperty, new DoubleAnimation(.35, 1, duration));
        var translate = new TranslateTransform(); view.RenderTransform = translate;
        translate.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(7, 0, duration) { EasingFunction = easing });
    }

    void GraphicsTab_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;
        string tab = button.CommandParameter?.ToString() ?? "Upscaling";
        foreach (var entry in new[] { ("Upscaling", GraphicsUpscalingTab, UpscalingPanel),
            ("Effects", GraphicsEffectsTab, EffectsPanel), ("Camera", GraphicsCameraTab, CameraPanel) })
        {
            bool selected = tab == entry.Item1;
            entry.Item2.Tag = selected ? "active" : null;
            entry.Item3.Visibility = selected ? Visibility.Visible : Visibility.Collapsed;
            if (selected) AnimateHub(entry.Item3);
        }
    }

    void ControlsTab_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;
        bool keyboard = button.CommandParameter?.ToString() != "Gamepad";
        KeyboardTab.Tag = keyboard ? "active" : null; GamepadTab.Tag = keyboard ? null : "active";
        KeyboardPanel.Visibility = keyboard ? Visibility.Visible : Visibility.Collapsed;
        GamepadPanel.Visibility = keyboard ? Visibility.Collapsed : Visibility.Visible;
        AnimateHub(keyboard ? KeyboardPanel : GamepadPanel);
    }

    void Provider_Click(object sender, RoutedEventArgs e)
    { if (!syncing && hubReady && sender is RadioButton button) SetComboTag(UpscalerCombo, button.Tag?.ToString() ?? "off"); }

    void UpdateHubSummary()
    {
        if (RenderScaleBox is null || ResolutionCombo.SelectedItem is null) return;
        string provider = ComboTag(UpscalerCombo);
        foreach (var button in ProviderGrid.Children.OfType<RadioButton>()) button.IsChecked = button.Tag?.ToString() == provider;
        bool scaled = provider is "dlss" or "fsr3" or "fsr4" or "fsr411";
        double factor = scaled ? ComboTag(PresetCombo) switch { "1" => 1.5, "2" => 1.7, "3" => 2, "4" => 3, _ => 1 } : 1;
        string output = ComboText(ResolutionCombo, "1920x1080");
        if (output == "auto")
        {
            var size = Techmo.Lantern.Core.Choices.OutputSize(output);
            output = $"{size.W}x{size.H}";
        }
        string[] dimensions = output.Split('x');
        if (dimensions.Length != 2 || !int.TryParse(dimensions[0], out int width) || !int.TryParse(dimensions[1], out int height)) return;
        // The runtime aligns the requested scene dimensions; this is a settings preview, not live telemetry.
        int sceneWidth = Math.Max(1, (int)Math.Round(width / factor / 2, MidpointRounding.AwayFromZero)) * 2;
        int sceneHeight = Math.Max(1, (int)Math.Round(height / factor / 2, MidpointRounding.AwayFromZero)) * 2;
        SceneResolutionText.Text = $"{sceneWidth} × {sceneHeight}";
        OutputResolutionText.Text = $"{width} × {height}";
        RenderScalePercent.Text = (100 / factor).ToString("0", CultureInfo.InvariantCulture) + "%";
        var transform = (ScaleTransform)RenderScaleBox.RenderTransform;
        foreach (var property in new[] { ScaleTransform.ScaleXProperty, ScaleTransform.ScaleYProperty })
        {
            double from = (double)transform.GetValue(property);
            transform.BeginAnimation(property, null); transform.SetValue(property, 1 / factor);
            if (MotionEnabled) transform.BeginAnimation(property, new DoubleAnimation(from, 1 / factor, TimeSpan.FromMilliseconds(250))
                { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
        }
        L10n.SetText(UpscalerDescription, provider switch
        {
            "off" => "Renders at the output resolution without temporal reconstruction.",
            "taa" => "Temporal anti-aliasing at the output resolution. No upscaling is applied.",
            "dlss" => "NVIDIA DLSS Super Resolution. Requires an RTX GPU and installed DLSS assets.",
            "fsr4" => "Legacy FSR 4 path. Requires its separate assets and a compatible GPU.",
            "fsr411" => "FSR 4.1.1 reconstruction. Requires its separate assets and a compatible GPU.",
            _ => "AMD FSR 3.1 reconstructs the scene at your output resolution."
        });
        L10n.SetText(UpscalingModeDescription, !scaled ? "Quality modes do not apply to Off or TAA." : ComboTag(PresetCombo) switch
        {
            "0" => "Native AA reconstructs at the output resolution without reducing the scene size.",
            "1" => "Quality renders each dimension at approximately 67% of the output resolution.",
            "2" => "Balanced renders each dimension at approximately 59% of the output resolution.",
            "3" => "Performance renders each dimension at 50% of the output resolution.",
            _ => "Ultra Performance renders each dimension at approximately 33% of the output resolution."
        });
        UpdateKeycapLabels();
    }

    void UpdatePresentModeDescription()
    {
        if (PresentModeHint is null) return;
        L10n.SetText(PresentModeHint, ComboTag(PresentCombo) switch
        {
            "Fifo" => "FIFO synchronizes presentation with the display refresh rate. Frames wait in order; tearing is avoided.",
            "FifoRelaxed" => "FIFO Relaxed synchronizes on time, but may tear when a frame misses the refresh deadline.",
            "Immediate" => "Immediate presents as soon as possible. Tearing can occur.",
            _ => "Mailbox uses the latest completed frame at each refresh. The driver may fall back if this mode is unavailable."
        });
    }

    void Keycap_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button key || keyBindings.FirstOrDefault(row => row.Key == key.CommandParameter?.ToString()) is not {} row) return;
        CaptureBinding_Click(new Button { DataContext = row, CommandParameter = "keyboard" }, e);
    }

    void UpdateKeycapLabels()
    {
        if (KeyForward is null) return;
        foreach (var button in new[] { KeyForward, KeyLeft, KeyBack, KeyRight, KeyRoll, KeyModifier })
        {
            var row = keyBindings.FirstOrDefault(r => r.Key == button.CommandParameter?.ToString());
            if (row is null) continue;
            button.Content = new TextBlock { Text = row.KeyboardDisplay, TextTrimming = TextTrimming.CharacterEllipsis,
                HorizontalAlignment = HorizontalAlignment.Center };
            button.ToolTip = row.Action + " · " + row.KeyboardDisplay;
            System.Windows.Automation.AutomationProperties.SetName(button, row.Action + " · " + row.KeyboardDisplay);
        }
    }

    void Appearance_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || !hubReady) return;
        state.UiTheme = ComboTag(ThemeCombo, "dark"); ApplyAppearance(); MarkSettingsChanged();
    }
    void AppearanceToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (syncing || !hubReady) return;
        state.ReducedMotion = ReducedMotionCheck.IsChecked == true;
        // Stop an in-flight transition as soon as reduced motion is enabled.
        if (state.ReducedMotion)
        {
            foreach (var view in new FrameworkElement[] { HomePage, DisplayPage, GraphicsPage, ControllerPage, ModsPage,
                OnlinePage, AdvancedPage, GeneralPage, UpscalingPanel, EffectsPanel, CameraPanel, KeyboardPanel, GamepadPanel }) AnimateHub(view);
            SetApplyBarVisible(applyBarShown, immediate: true);
        }
        MarkSettingsChanged();
    }
    void SystemAppearanceChanged(object sender, UserPreferenceChangedEventArgs e)
    { if (hubReady && state.UiTheme == "system") Dispatcher.BeginInvoke(new Action(ApplyAppearance)); }

    void ApplyAppearance()
    {
        bool light = state.UiTheme == "light";
        if (state.UiTheme == "system")
        {
            try { light = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize", "AppsUseLightTheme", 0) is int value && value != 0; }
            catch (Exception error) when (error is System.Security.SecurityException or UnauthorizedAccessException) { }
        }
        foreach (var (key, dark, bright) in new[]
        {
            ("AppBackgroundBrush", "#080808", "#F3F4F6"), ("SidebarBrush", "#0A0A0A", "#FAFAFB"),
            ("PopoverBrush", "#171717", "#FFFFFF"), ("PanelBrush", "#0DFFFFFF", "#FFFFFF"), ("RaisedBrush", "#12FFFFFF", "#F5F5F7"),
            ("InputBrush", "#0AFFFFFF", "#F0F1F4"), ("HoverBrush", "#1AFFFFFF", "#E9EAEE"),
            ("LineBrush", "#1FFFFFFF", "#DEE0E6"), ("TextBrush", "#F0F0F0", "#24252A"),
            ("MutedBrush", "#A5A5A5", "#656876"), ("DimBrush", "#858585", "#747783"),
            ("AccentBrush", "#EDEDED", "#F3A364"), ("AccentTextBrush", "#F5F5F5", "#964509"),
            ("AccentSoftBrush", "#16FFFFFF", "#FFF0E3"), ("PrimaryTextBrush", "#141414", "#2B1D14"),
            ("OverlayBrush", "#F2080808", "#E8F3F4F6")
        }) Application.Current.Resources[key] = new SolidColorBrush((Color)ColorConverter.ConvertFromString(light ? bright : dark));
    }
}
