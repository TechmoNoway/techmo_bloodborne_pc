// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace BloodborneLauncher;

public partial class MainWindow
{
    bool applyBarShown;
    int applyBarTransition;

    static double NormalizeSharpness(double value) => double.IsFinite(value)
        ? Math.Round(Math.Clamp(value, 0, 2), 2, MidpointRounding.AwayFromZero) : 0.3;

    void Sharpness_Changed(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateSharpnessUi();
    void Sharpening_Changed(object sender, RoutedEventArgs e) => UpdateSharpnessUi();

    void UpdateSharpnessUi()
    {
        if (SharpnessSlider is null || SharpnessValue is null || SharpnessControls is null || SharpenCheck is null) return;
        bool supported = ComboTag(UpscalerCombo) != "off";
        SharpenCheck.IsEnabled = supported;
        SharpnessControls.IsEnabled = supported && SharpenCheck.IsChecked == true;
        SharpnessValue.Text = NormalizeSharpness(SharpnessSlider.Value).ToString("0.00", CultureInfo.InvariantCulture);
        if (SharpnessHint is not null) L10n.SetText(SharpnessHint, supported
            ? "Adjust image sharpness. High values can emphasize noise and bright edges. Apply changes, then restart the game."
            : "Sharpening is unavailable with Upscaler Off. Your strength is kept for TAA, DLSS or FSR.");
    }

    void Settings_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (e.Handled || captureRow is not null) return;
        static DependencyObject? Parent(DependencyObject value) => value is Visual
            ? VisualTreeHelper.GetParent(value) : LogicalTreeHelper.GetParent(value);
        ComboBox? combo = null;
        for (var node = e.OriginalSource as DependencyObject; node is not null; node = Parent(node))
            if (node is ComboBox found) { combo = found; break; }
        // An open list still scrolls normally. A closed field must never change under
        // the pointer while the user is just trying to read the settings below it.
        if (combo is null || combo.IsDropDownOpen) return;
        e.Handled = true;
        for (var node = Parent(combo); node is not null; node = Parent(node))
            if (node is ScrollViewer scroll)
            {
                scroll.RaiseEvent(new MouseWheelEventArgs(e.MouseDevice, e.Timestamp, e.Delta)
                    { RoutedEvent = Mouse.MouseWheelEvent });
                break;
            }
    }

    void SetFooterMessage(string message, params object[] arguments)
    {
        L10n.SetText(FooterMessage, message, arguments);
        // Errors and session notices remain visible even when there is no pending edit.
        SetApplyBarVisible(true);
    }

    void SetApplyBarVisible(bool visible, bool immediate = false)
    {
        if (ApplyBarViewport is null || ApplyBarTranslate is null) return;
        ApplySettingsButton.IsEnabled = hasUnsavedChanges;
        UndoSettingsButton.IsEnabled = hasUnsavedChanges && appliedSettings is not null;
        if (applyBarShown == visible && !immediate) return;
        applyBarShown = visible;
        int transition = ++applyBarTransition;
        double fromHeight = ApplyBarViewport.Height, fromY = ApplyBarTranslate.Y;
        double height = ApplyBar.Height;
        ApplyBarViewport.BeginAnimation(HeightProperty, null);
        ApplyBarTranslate.BeginAnimation(TranslateTransform.YProperty, null);
        ApplyBarViewport.Height = visible ? height : 0;
        ApplyBarTranslate.Y = visible ? 0 : height;
        ApplyBarViewport.IsHitTestVisible = visible;
        if (!visible && ApplyBar.IsKeyboardFocusWithin) Focus();
        if (immediate || !MotionEnabled)
        {
            ApplyBarViewport.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            return;
        }
        ApplyBarViewport.Visibility = Visibility.Visible;
        var duration = TimeSpan.FromMilliseconds(220);
        var easing = new CubicEase { EasingMode = EasingMode.EaseOut };
        var reveal = new DoubleAnimation(fromHeight, visible ? height : 0, duration) { EasingFunction = easing };
        reveal.Completed += (_, _) =>
        {
            if (transition == applyBarTransition && !applyBarShown) ApplyBarViewport.Visibility = Visibility.Collapsed;
        };
        ApplyBarViewport.BeginAnimation(HeightProperty, reveal);
        ApplyBarTranslate.BeginAnimation(TranslateTransform.YProperty,
            new DoubleAnimation(fromY, visible ? 0 : height, duration) { EasingFunction = easing });
    }
}
