using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Windows;

namespace BloodborneLauncher;

public partial class MainWindow
{
#if TECHMO_PUBLIC_RELEASE
    static bool ShowLastSessionCard => false;
#else
    static bool ShowLastSessionCard => true;
#endif

    string? latestSessionDirectory;

    void LoadLatestSession()
    {
        try
        {
            string logs = Path.Combine(root, "logs");
            if (!Directory.Exists(logs)) return;
            string? latest = Directory.EnumerateDirectories(logs)
                .Where(dir => File.Exists(Path.Combine(dir, "session.json")))
                .OrderByDescending(dir => Directory.GetLastWriteTimeUtc(dir)).FirstOrDefault();
            if (latest != null) ShowSessionReport(latest, 0);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    void ShowSessionReport(string directory, int processExitCode)
    {
        latestSessionDirectory = directory;
        LastSessionCard.Visibility = ShowLastSessionCard ? Visibility.Visible : Visibility.Collapsed;
        HomeSessionCard.Visibility = LastSessionCard.Visibility;
        string category = processExitCode == 0 ? "normal" : "abnormal_exit";
        string reason = processExitCode == 0 ? "normal_exit" : "unknown_exit";
        string code = $"0x{unchecked((uint)processExitCode):X8}";
        bool complete = false;
        try
        {
            string path = Path.Combine(directory, "session.json");
            if (File.Exists(path) && new FileInfo(path).Length < 4 * 1024 * 1024)
            {
                using var json = JsonDocument.Parse(File.ReadAllText(path));
                var report = json.RootElement;
                complete = report.TryGetProperty("status", out var status) && status.GetString() == "finished";
                if (complete)
                {
                    var outcome = report.GetProperty("outcome");
                    category = outcome.GetProperty("category").GetString() ?? category;
                    reason = outcome.GetProperty("reason").GetString() ?? reason;
                    code = report.GetProperty("exit_code_hex").GetString() ?? code;
                }
                if (report.TryGetProperty("performance", out var performance) &&
                    performance.TryGetProperty("available", out var available) && available.GetBoolean())
                {
                    L10n.SetText(LastSessionPerformance, "Recorded session: {0} FPS average · {1} FPS 1% low · p99 {2} ms",
                        performance.GetProperty("average_fps").GetDouble().ToString("0.0", CultureInfo.InvariantCulture),
                        performance.GetProperty("one_percent_low_fps").GetDouble().ToString("0.0", CultureInfo.InvariantCulture),
                        performance.GetProperty("p99_frame_ms").GetDouble().ToString("0.00", CultureInfo.InvariantCulture));
                }
                else L10n.SetText(LastSessionPerformance, "No frame recording in this session.");
            }
            else L10n.SetText(LastSessionPerformance, "No frame recording in this session.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or KeyNotFoundException)
        {
            L10n.SetText(LastSessionPerformance, "The session report could not be read. Open the log for details.");
        }
        // A stale 'running' report is evidence of an interrupted reporting pipeline.
        if (!complete && File.Exists(Path.Combine(directory, "session.json")))
        {
            category = "abnormal_exit";
            reason = "incomplete_report";
        }
        string description = reason switch
        {
            "access_violation" => "Memory access violation; the underlying cause needs investigation.",
            "illegal_instruction" => "An unsupported or invalid CPU instruction was encountered.",
            "stack_overflow" => "A thread exhausted its stack.",
            "divide_by_zero" => "An integer division by zero occurred.",
            "device_lost" => "Vulkan reported device lost; the log is needed to investigate why.",
            "device_memory_error" => "Vulkan could not allocate device memory.",
            "renderer_assertion" => "The renderer stopped at an internal assertion; inspect the GPU log and crash dump.",
            "missing_dependency" => "A required native DLL could not be loaded.",
            "invalid_binary" => "Windows could not load a native executable or DLL.",
            "preparation_failed" => "Game preparation failed before the runtime started.",
            "interrupted" => "The session was interrupted.",
            "incomplete_report" => "The session ended before its report was completed.",
            "normal_exit" => "The runtime exited normally.",
            _ => "The process exited unexpectedly; the cause is not established."
        };
        if (category == "normal")
        {
            L10n.SetText(LastSessionStatus, "Bloodborne closed");
            SetFooterMessage("Bloodborne closed");
            LastSessionStatus.Foreground = Green();
        }
        else
        {
            L10n.SetText(LastSessionStatus, category == "startup_error" ? "Bloodborne could not start" : "Bloodborne stopped unexpectedly");
            SetFooterMessage("Session ended · {0}", code);
            LastSessionStatus.Foreground = Red();
        }
        L10n.SetText(LastSessionDetail, "{0}\nExit code: {1}", description, code);
    }

    void OpenLastSession_Click(object sender, RoutedEventArgs e)
    {
        if (latestSessionDirectory != null && Directory.Exists(latestSessionDirectory))
            Process.Start(new ProcessStartInfo(latestSessionDirectory) { UseShellExecute = true });
    }
}
