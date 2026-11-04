// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Windows.Threading;
using N = Techmo.Engine.Core;

namespace BloodborneLauncher;

public partial class MainWindow
{
    DispatcherTimer? engineSessionTimer;
    StreamReader? engineSessionReader;
    string? engineSessionLog;
    string engineSessionPending = "";
    DateTime? engineSessionStartedUtc;

    void StartEngineSession(string logPath)
    {
        if (!engineOptionsReady) return;
        StopEngineSession();
        engineSessionLog = logPath;
        engineSessionStartedUtc = DateTime.UtcNow;
        engineSessionTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        engineSessionTimer.Tick += (_, _) => PollEngineSession();
        engineSessionTimer.Start();
        UpdateEnginePresence();
    }

    void UpdateEnginePresence()
    {
        if (engineOptionsReady)
            N.DiscordPresence.Update(N.Workspace.Current.Settings, gameRunning, engineSessionStartedUtc);
    }

    void PollEngineSession()
    {
        if (engineSessionLog == null) return;
        try
        {
            engineSessionReader ??= new StreamReader(new FileStream(engineSessionLog, FileMode.Open,
                FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            var buffer = new char[8192];
            // Bound work per tick, retaining incomplete lines until the next poll.
            for (int chunk = 0; chunk < 8; chunk++)
            {
                int count = engineSessionReader.Read(buffer, 0, buffer.Length);
                if (count == 0) break;
                engineSessionPending += new string(buffer, 0, count);
                int end;
                while ((end = engineSessionPending.IndexOf('\n')) >= 0)
                {
                    string line = engineSessionPending[..end].TrimEnd('\r');
                    engineSessionPending = engineSessionPending[(end + 1)..];
                    if (line.StartsWith(N.LiveGame.Prefix, StringComparison.Ordinal)) N.LiveGame.Take(line);
                }
                if (engineSessionPending.Length > 8192) engineSessionPending = "";
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or FormatException or OverflowException)
        { /* A missing/partial log must never interrupt a running game. */ }
    }

    void StopEngineSession()
    {
        engineSessionTimer?.Stop(); engineSessionTimer = null;
        engineSessionReader?.Dispose(); engineSessionReader = null;
        engineSessionLog = null; engineSessionPending = ""; engineSessionStartedUtc = null;
        N.LiveGame.Clear();
        if (engineOptionsReady) N.DiscordPresence.Update(N.Workspace.Current.Settings, false, null);
    }
}
