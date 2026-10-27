using System.IO;
using Techmo.Engine.Build;

namespace Techmo.Engine.Core
{
    /// <summary>
    /// Where things live. The package folder (PortDir) holds the launcher, Bloodborne.exe and the
    /// player's files - bbport.ini, mods.json, patches.json, user, mods, logs (its data folder,
    /// BB_DATA_DIR) - and the engine in engine\: out\bb-probe.exe, scripts\run_windows.py and a
    /// bundled Python. Older packages had the engine at the top (EngineDirOf handles both).
    /// Launcher settings are per-user, in %APPDATA%\bbport-launcher\settings.json.
    /// </summary>
    public static class Paths
    {
        // Development settings live with this checkout, independently of installed Lantern.
        public static readonly string ExperimentRoot = FindExperimentRoot();
        public static bool IsExperiment => File.Exists(Path.Combine(ExperimentRoot, "engine.lock.json"));
        public static readonly string ConfigDir = Path.Combine(ExperimentRoot, "user", "launcher");

        private static string FindExperimentRoot()
        {
            if (Environment.GetEnvironmentVariable("TECHMO_EXPERIMENT_ROOT") is { Length: > 0 } fixture)
                return Path.GetFullPath(fixture);
            foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
                for (var dir = new DirectoryInfo(start); dir != null; dir = dir.Parent)
                    if (File.Exists(Path.Combine(dir.FullName, "engine.lock.json"))) return dir.FullName;
            return AppContext.BaseDirectory;
        }

        public static readonly string ConfigFile = Path.Combine(ConfigDir, "settings.json");

        /// <summary>The port folder in use: the "port_dir" setting, else one found by ResolvePort.</summary>
        public static string PortDir { get; set; } = "";

        public static string DataDir =>
            Environment.GetEnvironmentVariable("BB_DATA_DIR") is { Length: > 0 } dir ? dir : PortDir;

        public static string IniPath =>
            Environment.GetEnvironmentVariable("BB_CONFIG") is { Length: > 0 } path
                ? path
                : Path.Combine(DataDir, "bbport.ini");

        /// <summary>A package's engine folder: its engine\, or the package itself (older layout).</summary>
        public static string EngineDirOf(string dir) =>
            File.Exists(Path.Combine(dir, "engine", "out", "bb-probe.exe")) ||
            File.Exists(Path.Combine(dir, "engine", "scripts", "run_windows.py"))
                ? Path.Combine(dir, "engine")
                : dir;

        /// <summary>The engine in use: out\bb-probe.exe, scripts, patches, shaders and Python.</summary>
        public static string EngineDir => EngineDirOf(PortDir);

        /// <summary>The engine and its DLLs.</summary>
        public static string BinariesDir => Path.Combine(EngineDir, "out");

        /// <summary>The Bloodborne PC engine: out\bb-probe.exe and what starts it (out\bb-start.exe, or
        /// scripts\run_windows.py before 0.9.32), in the folder or its engine\.</summary>
        public static string? DlssRuntime
        {
            get
            {
                if (OperatingSystem.IsWindows())
                {
                    var dll = Path.Combine(BinariesDir, "nvngx_dlss.dll");
                    return File.Exists(dll) ? dll : null;
                }
                try
                {
                    return Directory.Exists(BinariesDir)
                        ? Directory.GetFiles(BinariesDir, "libnvidia-ngx-dlss.so.*").OrderBy(f => f, StringComparer.Ordinal).LastOrDefault()
                        : null;
                }
                catch (IOException)
                {
                    return null;
                }
            }
        }

        /// <summary>The DLSS runtime's version: the DLL's file version, or the end of the .so's name.</summary>
        public static string DlssVersion(string runtime) => OperatingSystem.IsWindows()
            ? System.Diagnostics.FileVersionInfo.GetVersionInfo(runtime).FileVersion ?? ""
            : Path.GetFileName(runtime)["libnvidia-ngx-dlss.so.".Length..];

        /// <summary>The Bloodborne PC engine: out\bb-probe.exe and what starts it (out\bb-start.exe, or
        /// scripts\run_windows.py before 0.9.32), in the folder or its engine\.</summary>
        public static bool IsPortDir(string? dir)
        {
            if (string.IsNullOrWhiteSpace(dir)) return false;
            var engine = EngineDirOf(dir);
            return File.Exists(Path.Combine(engine, "out", "bb-probe.exe")) &&
                   (File.Exists(Path.Combine(engine, "out", "bb-start.exe")) ||
                    File.Exists(Path.Combine(engine, "scripts", "run_windows.py")));
        }

        /// <summary>The native start (0.9.32 and later): the boot image, mods, patches, then the game.</summary>
        public static string Start => Path.Combine(EngineDir, "out", "bb-start.exe");

        /// <summary>The Python an older engine started with (scripts\run_windows.py).</summary>
        public static string Python => new[] {
            Path.Combine(EngineDir, "tools", "python", "python.exe"),
            Path.Combine(ExperimentRoot, "builds", "_internal", "toolchain", "clang64", "bin", "python.exe"),
            Path.GetFullPath(Path.Combine(ExperimentRoot, "..", "techmo_launcher", "builds", "_internal", "toolchain", "clang64", "bin", "python.exe")),
            Path.Combine(Environment.GetEnvironmentVariable("BB_MSYS2") ?? @"C:\msys64", "clang64", "bin", "python.exe")
        }.FirstOrDefault(File.Exists) ?? "python.exe";

        /// <summary>The port next to the game folder (its usual layout: &lt;port&gt;\CUSA03173).</summary>
        public static string? PortBesideGame(string gameDir)
        {
            try
            {
                var parent = string.IsNullOrWhiteSpace(gameDir) ? null : Directory.GetParent(gameDir)?.FullName;
                return IsPortDir(parent) ? parent : null;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>A port among the folders beside the game folder (E:\games\CUSA03173 next to
        /// E:\games\bbport-windows), the one with the newest bb-probe.exe.</summary>
        public static string? PortNearGame(string gameDir)
        {
            try
            {
                var parent = string.IsNullOrWhiteSpace(gameDir) ? null : Directory.GetParent(gameDir)?.FullName;
                if (parent == null) return null;
                return Directory.GetDirectories(parent)
                    .Where(IsPortDir)
                    .OrderByDescending(dir => new[] { "bin", "out" }
                        .Select(b => new FileInfo(Path.Combine(EngineDirOf(dir), b, "bb-probe.exe")))
                        .Where(f => f.Exists).Select(f => f.LastWriteTimeUtc).DefaultIfEmpty(DateTime.MinValue).Max())
                    .FirstOrDefault();
            }
            catch
            {
                return null;
            }
        }

        /// <summary>A port folder above this exe (the launcher copied into the port).</summary>
        public static string? DetectPortDir()
        {
            if (IsPortDir(ExperimentRoot)) return ExperimentRoot;
            foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
                for (var dir = new DirectoryInfo(start); dir != null; dir = dir.Parent)
                    if (IsPortDir(dir.FullName))
                        return dir.FullName.TrimEnd(Path.DirectorySeparatorChar);
            return null;
        }

        /// <summary>launcher/windows in the source checkout, when this copy was built from it.</summary>
        public static string? SourceRoot => SourceStamp.LocateSources(AppContext.BaseDirectory);

        public static string Expand(string path) =>
            Environment.ExpandEnvironmentVariables(path.StartsWith("~")
                ? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + path[1..]
                : path);

        public static void OpenFolder(string path)
        {
            try
            {
                Directory.CreateDirectory(path);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"\"{path}\"")
                    { UseShellExecute = true });
            }
            catch
            {
                // Nothing useful to do if Explorer will not open.
            }
        }
    }
}
