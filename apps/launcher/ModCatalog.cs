// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace BloodborneLauncher;

internal sealed class ModSelection
{
    [JsonPropertyName("order")] public List<string> Order { get; set; } = new();
    [JsonPropertyName("disabled")] public List<string> Disabled { get; set; } = new();
    [JsonPropertyName("legacy_enabled")] public bool LegacyEnabled { get; set; } = true;
    [JsonPropertyName("direct_enabled")] public bool DirectEnabled { get; set; } = true;
    [JsonExtensionData] public Dictionary<string, JsonElement>? Extra { get; set; }

    internal ModSelection Copy() => JsonSerializer.Deserialize<ModSelection>(JsonSerializer.Serialize(this))!;
    internal bool Enabled(ModInfo info) => info.Kind switch
    {
        "legacy" => LegacyEnabled, "direct" => DirectEnabled,
        _ => !Disabled.Contains(info.Name, StringComparer.Ordinal)
    };

    internal static ModSelection Read(string path)
    {
        if (!File.Exists(path)) return new();
        if (new FileInfo(path).Length > 1024 * 1024) throw new InvalidDataException("Mod configuration is too large");
        var result = JsonSerializer.Deserialize<ModSelection>(File.ReadAllText(path))
            ?? throw new InvalidDataException("Mod configuration must be an object");
        if (result.Order is null || result.Disabled is null ||
            result.Order.Concat(result.Disabled).Any(name => name is null))
            throw new InvalidDataException("Mod order and disabled entries must be lists of folder names");
        return result;
    }
    internal void Save(string path) => AtomicSettingsFile.Write(path,
        JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }), keepBackup: true);
}

internal sealed class ModInfo
{
    public string Name { get; set; } = "";
    public string Path { get; set; } = "";
    public string Kind { get; set; } = "";
    public string[] Files { get; set; } = Array.Empty<string>();
    public string Error { get; set; } = "";
    public string Detail { get; set; } = "";
    internal string Id => Kind + ":" + Name;
    internal bool CanToggle => (Kind == "legacy" && Error == "layout") ||
        (Kind != "unsupported" && Error is "" or "invalid");
}

internal sealed class ModCatalog
{
    public List<ModInfo> Mods { get; set; } = new();
    public bool Direct { get; set; }

    internal static async Task<ModCatalog> Scan(string root, string modsDirectory, string game)
    {
        string? python = new[] { Path.Combine(root, "tools", "python", "python.exe"),
            Path.Combine(root, "builds", "_internal", "toolchain", "clang64", "bin", "python.exe"),
            Path.GetFullPath(Path.Combine(root, "..", "techmo_launcher", "builds", "_internal", "toolchain", "clang64", "bin", "python.exe")),
            Path.Combine(Environment.GetEnvironmentVariable("BB_MSYS2") ?? @"C:\msys64", "clang64", "bin", "python.exe") }
            .FirstOrDefault(File.Exists);
        if (python is null) throw new IOException("Python runtime not found");
        var start = new ProcessStartInfo(python)
        {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (string argument in new[] { Path.Combine(root, "scripts", "mods.py"), game,
            "--mods-dir", modsDirectory, "--inspect" }) start.ArgumentList.Add(argument);
        start.Environment["PYTHONUTF8"] = "1";
        using var process = Process.Start(start) ?? throw new IOException("Could not start mod scanner");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        try
        {
            var output = ReadBounded(process.StandardOutput, 32 * 1024 * 1024, timeout.Token);
            var errors = ReadBounded(process.StandardError, 64 * 1024, timeout.Token);
            await Task.WhenAll(output, errors, process.WaitForExitAsync(timeout.Token));
            if (process.ExitCode != 0) throw new IOException((await errors).Trim());
            string json = await output;
            return await Task.Run(() => JsonSerializer.Deserialize<ModCatalog>(json,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                ?? throw new InvalidDataException("Invalid mod catalog"));
        }
        catch
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            throw;
        }
    }

    static async Task<string> ReadBounded(StreamReader reader, int limit, CancellationToken token)
    {
        var content = new StringBuilder();
        var buffer = new char[8192];
        int count;
        while ((count = await reader.ReadAsync(buffer.AsMemory(), token)) > 0)
        {
            if (content.Length + count > limit) throw new InvalidDataException("Mod catalog output is too large");
            content.Append(buffer, 0, count);
        }
        return content.ToString();
    }
}
