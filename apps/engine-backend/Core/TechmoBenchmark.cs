// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;

namespace Techmo.Engine.Core;

public static class TechmoBenchmark
{
    public static void Start()
    {
        try
        {
            Process.Start(new ProcessStartInfo("powershell.exe")
            {
                UseShellExecute = true,
                WorkingDirectory = Paths.ExperimentRoot,
                Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(Paths.ExperimentRoot, "tools", "benchmark.ps1")}\" -Target techmo-v2"
            });
        }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        { Workspace.Current.Log("Benchmark: " + error.Message, LogLevel.Bad); }
    }

    public static void Results() => Paths.OpenFolder(Path.GetFullPath(
        Path.Combine(Paths.ExperimentRoot, "..", "benchmarks", "results")));
}
