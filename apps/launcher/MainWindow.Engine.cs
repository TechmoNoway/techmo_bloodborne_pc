// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;
using System.Windows;

namespace BloodborneLauncher;

public partial class MainWindow
{
    // Defaults from the pinned Engine 0.9.34 launcher, not Techmo's v1 renderer experiments.
    void ConfigureEngineLaunch(ProcessStartInfo psi)
    {
        if (engineOptionsReady)
        {
            ConfigureHostedEngineLaunch(psi);
            psi.Environment["BB_GPU_USER_DIR"] = Path.Combine(root, "user");
            return;
        }
        psi.Environment["BB_USER_DIR"] = Path.Combine(root, "user");
        psi.Environment["BB_GPU_USER_DIR"] = Path.Combine(root, "user");
        psi.Environment["BB_PATCHES_DIR"] = Path.Combine(root, "patches");
        psi.Environment["BB_PATCHES_CONFIG"] = Path.Combine(root, "patches.json");
        psi.Environment["BB_NATIVE_SHADERS"] = "0";
        psi.Environment["BB_NATIVE_CALLS"] = "0";
        psi.Environment["BB_OCCLUSION"] = "0";
        psi.Environment["BB_PRECISE_PACING"] = "0";
        psi.Environment["BB_GPU_THREAD_PRIORITY"] = "0";
        psi.Environment["BB_SLEEP_SPIN_US"] = "600";
        psi.Environment["BB_AUDIO_THREAD_PRIORITY"] = "0";
        psi.Environment["BB_ANISO"] = "16";
        psi.Environment["BB_BACKGROUND_FPS"] = "30";
        psi.Environment["BB_SHARP_UI"] = "0";
        psi.Environment["BB_IMAGE_WRITEBACK_GC"] = "1";
        psi.Environment["BB_VBLANK_HZ"] = state.Fps is "30" or "60" ? "60" : state.Fps == "90" ? "90" : "0";
        psi.Environment.Remove("BB_DIRECT_IMAGE_BARRIERS");
        psi.Environment["SDL_JOYSTICK_HIDAPI_PS3"] = "1";
        psi.Environment["SDL_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER"] = "1";
    }

    void Benchmark_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Process.Start(new ProcessStartInfo("powershell.exe")
            {
                UseShellExecute = true,
                WorkingDirectory = root,
                Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(root, "tools", "benchmark.ps1")}\" -Target techmo-v2"
            });
        }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        { MessageBox.Show(error.Message, "Benchmark", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    void OpenBenchmarks_Click(object sender, RoutedEventArgs e)
    {
        string results = Path.GetFullPath(Path.Combine(root, "..", "benchmarks", "results"));
        Directory.CreateDirectory(results);
        Process.Start(new ProcessStartInfo(results) { UseShellExecute = true });
    }
}
