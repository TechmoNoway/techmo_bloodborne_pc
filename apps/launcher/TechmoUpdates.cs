// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace BloodborneLauncher;

public sealed record TechmoRelease(Version Version, string Tag, string Notes, string AssetName,
    string DownloadUrl, long Size, string? Sha256, string? ChecksumsUrl);
public sealed record UpdateProgress(string Phase, double Fraction);
public sealed record UpdateFile(string Path, string Sha256);
public sealed record PreparedUpdate(string Root, string Stage, string Version, IReadOnlyList<UpdateFile> Files);

public sealed class TechmoUpdates(HttpClient http)
{
    public const string Repository = "TechmoNoway/techmo_bloodborne_pc";
    public static Version Current => typeof(MainWindow).Assembly.GetName().Version is { } v
        ? new(v.Major, v.Minor, Math.Max(v.Build, 0)) : new(0, 0, 0);
    public static string CurrentLabel => Current.Build == 0 ? $"{Current.Major}.{Current.Minor}" : Current.ToString(3);
    public static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"Techmo-Bloodborne/{Current}");
        return client;
    }

    public async Task<TechmoRelease?> LatestAsync(CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        using var response = await http.GetAsync($"https://api.github.com/repos/{Repository}/releases/latest", timeout.Token);
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        response.EnsureSuccessStatusCode();
        return Parse(await response.Content.ReadAsStringAsync(timeout.Token));
    }

    static bool DownloadUrl(string url, string tag) => Uri.TryCreate(url, UriKind.Absolute, out var uri) &&
        uri.Scheme == "https" && uri.Host == "github.com" && uri.UserInfo.Length == 0 &&
        uri.AbsolutePath.StartsWith($"/{Repository}/releases/download/{tag}/", StringComparison.Ordinal);
    static string? Digest(string? text) => text != null && Regex.IsMatch(text, "^[0-9a-fA-F]{64}$") ? text.ToLowerInvariant() : null;

    public static TechmoRelease? Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var r = doc.RootElement;
        if (r.GetProperty("draft").GetBoolean() || r.GetProperty("prerelease").GetBoolean()) return null;
        string tag = r.GetProperty("tag_name").GetString() ?? "";
        if (!Regex.IsMatch(tag, @"^v?\d+\.\d+(\.\d+)?$") || !Version.TryParse(tag.TrimStart('v'), out var parsed)) return null;
        var version = new Version(parsed.Major, parsed.Minor, Math.Max(parsed.Build, 0));
        var name = $"Techmo-Bloodborne-{tag.TrimStart('v')}-win-x64.zip";
        string? url = null, sha = null, sums = null;
        long size = 0;
        foreach (var asset in r.GetProperty("assets").EnumerateArray())
        {
            var file = asset.GetProperty("name").GetString();
            var address = asset.GetProperty("browser_download_url").GetString() ?? "";
            if (!DownloadUrl(address, tag)) continue;
            if (file == name)
            {
                url = address; size = asset.GetProperty("size").GetInt64();
                sha = asset.TryGetProperty("digest", out var d) ? Digest(d.GetString()?.Replace("sha256:", "")) : null;
            }
            if (file == "SHA256SUMS.txt") sums = address;
        }
        if (url == null || size <= 0 || size > 2L * 1024 * 1024 * 1024 || (sha == null && sums == null)) return null;
        return new(version, tag, r.TryGetProperty("body", out var body) ? body.GetString() ?? "" : "", name, url, size, sha, sums);
    }

    public static bool CanInstall(string root, string? executable) =>
        !File.Exists(Path.Combine(root, ".git")) && !Directory.Exists(Path.Combine(root, ".git")) &&
        executable != null && Path.GetFullPath(executable).Equals(
            Path.GetFullPath(Path.Combine(root, "builds/launcher/Techmo.Launcher.exe")), StringComparison.OrdinalIgnoreCase);

    public static bool InstallInProgress(string root)
    {
        string path = Path.Combine(root, ".techmo-update/install.lock");
        if (!File.Exists(path)) return false;
        try { using var held = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None); return false; }
        catch (IOException) { return true; }
    }

    public static bool RuntimePath(string path)
    {
        var parts = path.Split('/');
        if (parts.Length == 1) return new[] { "Techmo.cmd", "run.bat", "build.sh", "engine.lock.json", "package.json", "LICENSE", "README.md" }.Contains(path, StringComparer.OrdinalIgnoreCase);
        return parts[0].ToLowerInvariant() switch
        {
            "out" or "scripts" or "licenses" or "docs" or "patches" or "data" => true,
            "builds" => parts.Length > 2 && parts[1].Equals("launcher", StringComparison.OrdinalIgnoreCase),
            "tools" => parts[1].Equals("python", StringComparison.OrdinalIgnoreCase) ||
                (parts.Length == 2 && new[] { "benchmark.ps1", "benchmark.py", "install-update.ps1" }.Contains(parts[1], StringComparer.OrdinalIgnoreCase)),
            _ => false
        };
    }

    public static string SafePath(string root, string relative)
    {
        if (relative.Length == 0 || relative.Contains('\\') || relative.Split('/').Any(p =>
            p.Length == 0 || p is "." or ".." || p.EndsWith('.') || p.EndsWith(' ') || p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0))
            throw new InvalidDataException("Invalid update path.");
        var full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        var fence = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(fence, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Update path escaped its folder.");
        for (string? item = full; item != null && item.StartsWith(fence, StringComparison.OrdinalIgnoreCase); item = Path.GetDirectoryName(item))
            if ((File.Exists(item) || Directory.Exists(item)) && (File.GetAttributes(item) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("An update path is a symbolic link or junction.");
        if (Directory.Exists(root) && (File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("The update folder is a symbolic link or junction.");
        return full;
    }

    public async Task<PreparedUpdate> PrepareAsync(TechmoRelease release, string root, IProgress<UpdateProgress>? progress, CancellationToken token)
    {
        if (File.Exists(Path.Combine(root, ".git")) || Directory.Exists(Path.Combine(root, ".git")))
            throw new IOException("Use an extracted release package to install updates; source checkouts are built separately.");
        var stage = SafePath(root, ".techmo-update/" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(stage);
        try
        {
            var expected = release.Sha256;
            if (expected == null)
            {
                var sums = await http.GetStringAsync(release.ChecksumsUrl!, token);
                expected = sums.Split('\n').Select(line => Regex.Match(line.Trim(), @"^([0-9a-fA-F]{64})\s+\*?(.+)$"))
                    .Where(m => m.Success && m.Groups[2].Value == release.AssetName).Select(m => Digest(m.Groups[1].Value)).SingleOrDefault();
            }
            if (expected == null) throw new InvalidDataException("The release has no matching SHA256 checksum.");
            var archive = Path.Combine(stage, "download.zip");
            using (var response = await http.GetAsync(release.DownloadUrl, HttpCompletionOption.ResponseHeadersRead, token))
            {
                response.EnsureSuccessStatusCode();
                if (response.Content.Headers.ContentLength is long length && length != release.Size) throw new InvalidDataException("Download size mismatch.");
                await using var input = await response.Content.ReadAsStreamAsync(token);
                await using var output = File.Create(archive);
                var buffer = new byte[81920]; long done = 0; int count;
                while ((count = await input.ReadAsync(buffer, token)) != 0)
                {
                    token.ThrowIfCancellationRequested(); done += count;
                    if (done > release.Size) throw new InvalidDataException("Download is larger than expected.");
                    await output.WriteAsync(buffer.AsMemory(0, count), token);
                    progress?.Report(new("download", (double)done / release.Size));
                }
                if (done != release.Size) throw new InvalidDataException("Download is incomplete.");
            }
            await using (var stream = File.OpenRead(archive))
                if (!Convert.ToHexString(await SHA256.HashDataAsync(stream, token)).Equals(expected, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("SHA256 verification failed.");
            var prepared = await Task.Run(() => ExtractVerified(archive, release, root, stage, progress, token), token);
            File.Delete(archive);
            return prepared;
        }
        catch { DeleteStage(root, stage); throw; }
    }

    static PreparedUpdate ExtractVerified(string archive, TechmoRelease release, string root, string stage, IProgress<UpdateProgress>? progress, CancellationToken token)
    {
        var payload = Path.Combine(stage, "payload"); Directory.CreateDirectory(payload);
        var prefix = release.AssetName[..^"-win-x64.zip".Length] + "/";
        using var zip = ZipFile.OpenRead(archive);
        if (zip.Entries.Count > 30000 || zip.Entries.Sum(e => e.Length) > 4L * 1024 * 1024 * 1024) throw new InvalidDataException("Update package is too large.");
        var files = new List<UpdateFile>(); var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in zip.Entries)
        {
            token.ThrowIfCancellationRequested();
            if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal)) throw new InvalidDataException("Unexpected package root.");
            var relative = entry.FullName[prefix.Length..];
            if (relative.Length == 0 || relative.EndsWith('/')) continue;
            var target = SafePath(payload, relative);
            if (!seen.Add(relative) || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000) throw new InvalidDataException("Duplicate or linked update entry.");
            if (!RuntimePath(relative)) continue; // Never import settings, saves, mods or caches from a package.
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            entry.ExtractToFile(target);
            using var stream = File.OpenRead(target);
            files.Add(new(relative, Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant()));
            progress?.Report(new("verify", (double)seen.Count / zip.Entries.Count));
        }
        foreach (var path in new[] { "Techmo.cmd", "builds/launcher/Techmo.Launcher.exe", "tools/python/python.exe", "scripts/run_techmo.py", "out/bb-start.exe", "out/bb-probe.exe", "out/bb-gpu-capabilities.exe", "out/bb-engine-assets.exe", "out/bb-shader-lab.exe", "out/native-build.json", "package.json", "engine.lock.json" })
            if (!File.Exists(SafePath(payload, path))) throw new InvalidDataException("Update is missing " + path);
        using var package = JsonDocument.Parse(File.ReadAllText(Path.Combine(payload, "package.json")));
        using var native = JsonDocument.Parse(File.ReadAllText(Path.Combine(payload, "out/native-build.json")));
        if (!Version.TryParse(package.RootElement.GetProperty("version").GetString(), out var version) || version != release.Version ||
            native.RootElement.GetProperty("source_identity").GetProperty("app_version").GetString() != version.ToString(3))
            throw new InvalidDataException("The package version does not match the release.");
        foreach (var binary in native.RootElement.GetProperty("binaries").EnumerateObject())
        {
            string relative = "out/" + binary.Name;
            string? hash = files.SingleOrDefault(f => f.Path == relative)?.Sha256;
            if (hash == null || !hash.Equals(binary.Value.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Native binary verification failed: " + binary.Name);
        }
        foreach (var dll in native.RootElement.GetProperty("runtime_dlls").EnumerateObject())
            if (!files.Any(f => f.Path == "out/" + dll.Name && f.Sha256.Equals(dll.Value.GetString(), StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("Runtime DLL verification failed: " + dll.Name);
        return new(root, stage, version.ToString(3), files);
    }

    public static string WritePlan(PreparedUpdate update, int parentId, DateTime parentStartedUtc)
    {
        var path = Path.Combine(update.Stage, "plan.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new { update.Root, update.Stage, update.Version, parentId, parentStartedUtc, update.Files },
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        return path;
    }

    public static void StartInstaller(PreparedUpdate update)
    {
        if (!CanInstall(update.Root, Environment.ProcessPath)) throw new IOException("Run the launcher from an extracted release package to install updates.");
        var script = Path.Combine(update.Stage, "install.ps1");
        using var resource = typeof(TechmoUpdates).Assembly.GetManifestResourceStream("BloodborneLauncher.InstallUpdate.ps1")
            ?? throw new IOException("Update installer is missing.");
        using (var file = File.Create(script)) resource.CopyTo(file);
        using var current = Process.GetCurrentProcess();
        var plan = WritePlan(update, current.Id, current.StartTime.ToUniversalTime());
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell/v1.0/powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var arg in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", script, "-Plan", plan }) start.ArgumentList.Add(arg);
        using var helper = Process.Start(start) ?? throw new IOException("Could not start the update installer.");
    }

    public static void DeleteStage(string root, string stage)
    {
        var parent = Path.Combine(Path.GetFullPath(root), ".techmo-update");
        if (Path.GetDirectoryName(Path.GetFullPath(stage)) != parent || !Regex.IsMatch(Path.GetFileName(stage), "^[0-9a-f]{32}$"))
            throw new IOException("Invalid update staging directory.");
        SafePath(root, ".techmo-update/" + Path.GetFileName(stage));
        if (Directory.Exists(stage))
        {
            if (Directory.EnumerateFileSystemEntries(stage, "*", SearchOption.AllDirectories).Any(p => (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0))
                throw new IOException("Preserving linked update staging directory.");
            Directory.Delete(stage, true);
        }
    }
}
