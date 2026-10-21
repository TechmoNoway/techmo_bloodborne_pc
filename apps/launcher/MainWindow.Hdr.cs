// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Windows;

namespace BloodborneLauncher;

public partial class MainWindow
{
    void Hdr_Changed(object sender, RoutedEventArgs e) => UpdateHdrControls();

    void UpdateHdrControls()
    {
        if (HdrPeakCombo is null) return;
        bool enabled = HdrOutputCheck.IsChecked == true;
        HdrWhiteCombo.IsEnabled = HdrExpandCheck.IsEnabled = enabled;
        HdrPeakCombo.IsEnabled = enabled;
    }

    void ConfigureHdrLaunch(ProcessStartInfo psi)
    {
        // Lantern 0.9.34 uses HDR conversion keys different from Techmo v1.
        psi.Environment["BB_HDR"] = state.HdrOutput ? "1" : "0";
        psi.Environment["BB_HDR_PAPER_WHITE"] = state.HdrWhite is "80" or "100" or "150" or "203" or "250" or "300" or "400" ? state.HdrWhite : "203";
        psi.Environment["BB_HDR_PEAK"] = state.HdrPeak is "400" or "600" or "800" or "1000" or "1500" or "2000" ? state.HdrPeak : "1000";
        psi.Environment["BB_HDR_MODE"] = state.HdrExpand ? "expand" : "native";
        foreach (string retired in new[] { "BB_HDR_OUTPUT", "BB_HDR_WHITE", "BB_HDR_EXPAND" })
            psi.Environment.Remove(retired);
    }
}
