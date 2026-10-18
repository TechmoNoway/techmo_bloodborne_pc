// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Globalization;

namespace BloodborneLauncher;

public partial class MainWindow
{
    // The runtime still needs its startup hook switch, but users only choose the effect values.
    // A 100% multiplier leaves each area's original value intact; vignette On keeps the original.
    static bool HasCustomLighting(LauncherState value) => !value.GraphicsVignette ||
        value.GraphicsAo != "1.00" || value.GraphicsShadows != "1.00" ||
        value.GraphicsBloom != "1.00" || value.GraphicsSaturation != "1.00";

    static string LightingScale(Slider slider) =>
        (Math.Round(slider.Value, MidpointRounding.AwayFromZero) / 100).ToString("0.00", CultureInfo.InvariantCulture);

    void ApplyLightingUi()
    {
        GraphicsAoSlider.Value = double.Parse(state.GraphicsAo, CultureInfo.InvariantCulture) * 100;
        GraphicsShadowsSlider.Value = double.Parse(state.GraphicsShadows, CultureInfo.InvariantCulture) * 100;
        GraphicsBloomSlider.Value = double.Parse(state.GraphicsBloom, CultureInfo.InvariantCulture) * 100;
        GraphicsSaturationSlider.Value = double.Parse(state.GraphicsSaturation, CultureInfo.InvariantCulture) * 100;
        GraphicsVignetteCheck.IsChecked = state.GraphicsVignette;
        foreach (var slider in new[] { GraphicsAoSlider, GraphicsShadowsSlider, GraphicsBloomSlider, GraphicsSaturationSlider })
            UpdateLightingValue(slider);
    }

    void CollectLightingUi()
    {
        state.GraphicsAo = LightingScale(GraphicsAoSlider);
        state.GraphicsShadows = LightingScale(GraphicsShadowsSlider);
        state.GraphicsBloom = LightingScale(GraphicsBloomSlider);
        state.GraphicsSaturation = LightingScale(GraphicsSaturationSlider);
        state.GraphicsVignette = GraphicsVignetteCheck.IsChecked == true;
        state.GraphicsControls = HasCustomLighting(state);
    }

    void LightingSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    { if (sender is Slider slider) UpdateLightingValue(slider); }

    void UpdateLightingValue(Slider slider)
    {
        TextBlock? label = slider.Name switch
        {
            "GraphicsAoSlider" => GraphicsAoValue, "GraphicsShadowsSlider" => GraphicsShadowsValue,
            "GraphicsBloomSlider" => GraphicsBloomValue, "GraphicsSaturationSlider" => GraphicsSaturationValue,
            _ => null
        };
        if (label is null) return; // XAML may raise ValueChanged before the readout is constructed.
        int percentage = (int)Math.Round(slider.Value, MidpointRounding.AwayFromZero);
        if (percentage == 100) L10n.SetText(label, "100% — Original");
        else L10n.SetText(label, "{0}%", percentage);
    }

    void ResetLighting_Click(object sender, RoutedEventArgs e)
    {
        bool wasSyncing = syncing;
        syncing = true;
        try
        {
            foreach (var slider in new[] { GraphicsAoSlider, GraphicsShadowsSlider, GraphicsBloomSlider, GraphicsSaturationSlider })
                slider.Value = 100;
            GraphicsVignetteCheck.IsChecked = true;
        }
        finally { syncing = wasSyncing; }
        MarkSettingsChanged();
    }
}
