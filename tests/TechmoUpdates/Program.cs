using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using BloodborneLauncher;

static class Program
{
    static string source = "", fixture = "";
    static int checks;
    static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); checks++; }
    static string NewRoot()
    {
        string root = Path.Combine(fixture, "Máy chơi game ' & " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root); File.WriteAllText(Path.Combine(root, "Techmo.cmd"), "old launcher"); return root;
    }
    static readonly Dictionary<string, byte[]> Runtime = new()
    {
        ["Techmo.cmd"] = Encoding.UTF8.GetBytes("new launcher"),
        ["builds/launcher/Techmo.Launcher.exe"] = [77, 90, 1, 2],
        ["tools/python/python.exe"] = [77, 90, 3, 4],
        ["scripts/run_techmo.py"] = Encoding.UTF8.GetBytes("new wrapper"),
        ["engine.lock.json"] = Encoding.UTF8.GetBytes("{}"),
        ["out/bb-probe.exe"] = [77, 90, 5, 6],
        ["out/bb-start.exe"] = [77, 90, 7, 8],
        ["out/bb-gpu-capabilities.exe"] = [77, 90, 9],
        ["out/bb-engine-assets.exe"] = [77, 90, 10],
        ["out/bb-shader-lab.exe"] = [77, 90, 11],
        ["out/runtime.dll"] = [77, 90, 12],
    };
    static byte[] Archive(string? extra = null, bool missingDll = false, bool wrongVersion = false)
    {
        var entries = new Dictionary<string, byte[]>(Runtime);
        string version = wrongVersion ? "9.0.0" : "2.0.2";
        entries["package.json"] = JsonSerializer.SerializeToUtf8Bytes(new { version });
        entries["out/native-build.json"] = JsonSerializer.SerializeToUtf8Bytes(new
        {
            source_identity = new { app_version = version },
            binaries = Runtime.Where(p => p.Key.StartsWith("out/") && p.Key.EndsWith(".exe"))
                .ToDictionary(p => Path.GetFileName(p.Key), p => new { sha256 = Hash(p.Value) }),
            runtime_dlls = new Dictionary<string, string> { ["runtime.dll"] = Hash(Runtime["out/runtime.dll"]) }
        });
        if (missingDll) entries.Remove("out/runtime.dll");
        foreach (var path in new[] { "user/save.dat", "mods/mod.dat", "bbport.ini", "launcher-settings.json", "save-vault/backup.dat", ".git/config" })
            entries[path] = Encoding.UTF8.GetBytes("must never replace user data");
        if (extra != null) entries[extra] = [1, 2, 3];
        using var memory = new MemoryStream();
        using (var zip = new ZipArchive(memory, ZipArchiveMode.Create, true))
            foreach (var (path, data) in entries)
            {
                using var output = zip.CreateEntry("Techmo-Bloodborne-2.0.2/" + path).Open(); output.Write(data);
            }
        return memory.ToArray();
    }
    static string ReleaseJson(byte[] zip, bool digest = true, bool beta = false, bool draft = false, string repo = TechmoUpdates.Repository)
        => JsonSerializer.Serialize(new
        {
            tag_name = "v2.0.2", prerelease = beta, draft, body = "Changes",
            assets = new object[] {
                new { name="Techmo-Bloodborne-2.0.2-win-x64.zip", size=zip.Length, digest=digest?"sha256:"+Hash(zip):null,
                    browser_download_url=$"https://github.com/{repo}/releases/download/v2.0.2/Techmo-Bloodborne-2.0.2-win-x64.zip" },
                new { name="SHA256SUMS.txt",size=100,browser_download_url=$"https://github.com/{repo}/releases/download/v2.0.2/SHA256SUMS.txt" }
            }
        });
    sealed class Network(byte[] zip, string? metadata = null, HttpStatusCode status = HttpStatusCode.OK) : HttpMessageHandler
    {
        public readonly List<Uri> Requested = [];
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
        {
            Requested.Add(request.RequestUri!); token.ThrowIfCancellationRequested();
            var path = request.RequestUri!.AbsolutePath;
            HttpContent content = path.EndsWith(".zip") ? new ByteArrayContent(zip) :
                new StringContent(path.EndsWith("SHA256SUMS.txt") ? Hash(zip)+"  Techmo-Bloodborne-2.0.2-win-x64.zip\n" : metadata ?? ReleaseJson(zip));
            return Task.FromResult(new HttpResponseMessage(status) { Content = content });
        }
    }
    sealed class InlineProgress(Action<UpdateProgress> action) : IProgress<UpdateProgress> { public void Report(UpdateProgress p) => action(p); }
    static async Task<PreparedUpdate> Prepare(string root, byte[] zip, bool digest = true)
    {
        using var http = new HttpClient(new Network(zip));
        return await new TechmoUpdates(http).PrepareAsync(TechmoUpdates.Parse(ReleaseJson(zip, digest))!, root, null, default);
    }
    static async Task Reject(Func<Task> action, string message)
    {
        try { await action(); } catch (Exception e) when (e is IOException or InvalidDataException or OperationCanceledException or HttpRequestException) { checks++; return; }
        throw new Exception(message);
    }
    static async Task<int> Install(PreparedUpdate update, Process? parent = null)
    {
        string plan = TechmoUpdates.WritePlan(update, parent?.Id ?? 0, parent?.StartTime.ToUniversalTime() ?? DateTime.UtcNow);
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell/v1.0/powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var arg in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(source, "tools/install-update.ps1"), "-Plan", plan, "-NoRestart" }) start.ArgumentList.Add(arg);
        using var process = Process.Start(start)!;
        var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(35));
        File.WriteAllText(Path.Combine(update.Stage, "fixture-output.log"), await output + await error);
        return process.ExitCode;
    }
    static async Task Run()
    {
        byte[] zip = Archive();
        using (var embedded = typeof(TechmoUpdates).Assembly.GetManifestResourceStream("BloodborneLauncher.InstallUpdate.ps1")!)
        using (var bytes = new MemoryStream())
        {
            embedded.CopyTo(bytes);
            Check(Hash(bytes.ToArray()) == Hash(File.ReadAllBytes(Path.Combine(source,"tools/install-update.ps1"))), "The installed app embeds the tested helper exactly");
        }
        var release = TechmoUpdates.Parse(ReleaseJson(zip))!;
        Check(release.Version == new Version(2, 0, 2) && release.Version > new Version(2, 0, 1) && TechmoUpdates.Current.Major >= 2,
            "Release versions are normalized and the current version belongs to Techmo, not the engine backend");
        Check(TechmoUpdates.Parse(ReleaseJson(zip, beta:true)) == null, "Stable checks skip prereleases");
        Check(TechmoUpdates.Parse(ReleaseJson(zip, draft:true)) == null, "Draft releases are skipped");
        Check(TechmoUpdates.Parse(ReleaseJson(zip, repo:"Awakenchan/LanternEngine")) == null, "Upstream assets cannot be offered as Techmo updates");
        Check(!TechmoUpdates.CanInstall(source, Environment.ProcessPath), "Source checkout installation is blocked");
        using (var handler = new Network(zip))
        using (var net = new HttpClient(handler))
        {
            Check((await new TechmoUpdates(net).LatestAsync(default))?.Tag == "v2.0.2", "Release discovery works against injected HTTP responses");
            Check(handler.Requested.Single().AbsoluteUri == "https://api.github.com/repos/TechmoNoway/techmo_bloodborne_pc/releases/latest", "The update channel only queries Techmo releases");
        }
        using (var net = new HttpClient(new Network(zip, status:HttpStatusCode.NotFound)))
            Check(await new TechmoUpdates(net).LatestAsync(default) == null, "An unpublished release is an explicit empty result");
        var root = NewRoot();
        foreach (var p in new[] { "user/save.dat", "mods/mod.dat", "bbport.ini", "launcher-settings.json", "save-vault/backup.dat", "user/cache/cache.bin" })
        { Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(root,p))!); File.WriteAllText(Path.Combine(root,p),"personal data"); }
        var update = await Prepare(root, zip, digest:false);
        Check(!update.Files.Any(f => f.Path.StartsWith("user/") || f.Path == "bbport.ini" || f.Path.StartsWith(".git/")), "Personal and source data from ZIPs never enter the install plan");
        Check(update.Files.Count >= 12, "Full package is extracted and verified with checksum-file fallback");
        Check(await Install(update) == 0, "Actual PowerShell installer succeeds");
        Check(File.ReadAllText(Path.Combine(root,"Techmo.cmd")) == "new launcher", "Runtime files are replaced");
        Check(File.ReadAllText(Path.Combine(update.Stage,"backup/Techmo.cmd")) == "old launcher", "Previous runtime is backed up");
        Check(!Directory.Exists(Path.Combine(update.Stage,"payload")), "Successful install removes staged payload");
        foreach (var p in new[] { "user/save.dat", "mods/mod.dat", "bbport.ini", "launcher-settings.json", "save-vault/backup.dat", "user/cache/cache.bin" })
            Check(File.ReadAllText(Path.Combine(root,p)) == "personal data", "User data is unchanged: "+p);
        var second = await Prepare(root, zip);
        Check(await Install(second) == 0 && !Directory.Exists(update.Stage), "A subsequent successful install retains only the latest backup");

        var rollbackRoot = NewRoot();
        Directory.CreateDirectory(Path.Combine(rollbackRoot,"out"));
        string locked = Path.Combine(rollbackRoot,"out/bb-probe.exe"); File.WriteAllText(locked,"old engine");
        var rollback = await Prepare(rollbackRoot,zip);
        using (var held = new FileStream(locked,FileMode.Open,FileAccess.ReadWrite,FileShare.None))
            Check(await Install(rollback) != 0, "Locked destination causes an installation failure");
        Check(File.ReadAllText(Path.Combine(rollbackRoot,"Techmo.cmd")) == "old launcher" && File.ReadAllText(locked)=="old engine", "Failure rolls back previously replaced runtime files");
        Check(!File.Exists(Path.Combine(rollbackRoot,"builds/launcher/Techmo.Launcher.exe")), "Rollback removes new files introduced by the failed install");
        using (var result = JsonDocument.Parse(File.ReadAllText(Path.Combine(rollbackRoot,".techmo-update/result.json"))))
            Check(!result.RootElement.GetProperty("success").GetBoolean(), "Failed installation is reported to the next launcher start");

        foreach (var bad in new[] { Archive("../escape.txt"), Archive("OUT/BB-PROBE.EXE"), Archive("C:/escape.txt"), Archive(missingDll:true), Archive(wrongVersion:true) })
            await Reject(async () => { await Prepare(NewRoot(),bad); }, "Unsafe or incomplete package was accepted");
        using (var http = new HttpClient(new Network(zip)))
        {
            string failedRoot = NewRoot();
            await Reject(async () => { await new TechmoUpdates(http).PrepareAsync(release with { Sha256 = new string('0',64) },failedRoot,null,default); }, "Bad digest accepted");
            Check(!Directory.EnumerateDirectories(Path.Combine(failedRoot,".techmo-update")).Any(), "Failed verification cleans only its staging folder");
        }
        using (var http = new HttpClient(new Network(zip[..^5])))
            await Reject(async () => { await new TechmoUpdates(http).PrepareAsync(release,NewRoot(),null,default); }, "Truncated download accepted");
        using (var http = new HttpClient(new Network(zip)))
        using (var cancel = new CancellationTokenSource())
            await Reject(async () => { await new TechmoUpdates(http).PrepareAsync(release,NewRoot(),new InlineProgress(_=>cancel.Cancel()),cancel.Token); }, "Cancelled transfer continued");
        foreach (var relative in new[] { "../escape", "out/../../escape", "out/file:stream", "out/CON/..", "out/file. ", "out\\bad" })
        {
            try { TechmoUpdates.SafePath(root,relative); throw new Exception("Unsafe path accepted: "+relative); }
            catch (InvalidDataException) { checks++; }
        }
        var malicious = await Prepare(NewRoot(),zip);
        var injected = malicious with { Files = malicious.Files.Append(new UpdateFile("user/save.dat",new string('0',64))).ToArray() };
        Check(await Install(injected) != 0, "Installer independently rejects a plan that targets user data");
        Check(File.ReadAllText(Path.Combine(malicious.Root,"Techmo.cmd")) == "old launcher", "Invalid plans make no runtime changes");
        var waiting = await Prepare(NewRoot(),zip);
        var sleeper = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell/v1.0/powershell.exe"))
        { UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden };
        foreach (var arg in new[] { "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 5" }) sleeper.ArgumentList.Add(arg);
        using (var parent = Process.Start(sleeper)!)
        {
            Check(await Install(waiting,parent)==0 && parent.HasExited, "The helper waits for the original parent to close, including time-zone-safe PID identity");
            await parent.WaitForExitAsync();
        }
        Console.WriteLine($"PASS: {checks} update checks; release discovery, checksums, extraction, actual installation, rollback, data preservation and cancellation.");
    }
    static async Task<int> Main(string[] args)
    {
        source=Path.GetFullPath(args[0]); fixture=Path.Combine(source,"builds/_internal/update-fixtures-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(fixture);
        try
        {
            await Run();
            if (args.Contains("--live"))
            {
                using var http = TechmoUpdates.CreateClient();
                var live = await new TechmoUpdates(http).LatestAsync(default);
                Check(live != null && live.Version.Major >= 2, "The production client reads the real public Techmo release");
                Console.WriteLine("PASS: live Techmo release discovery: "+live!.Tag);
            }
            if (args.Length > 2 && args[1] == "--package")
            {
                byte[] bytes = File.ReadAllBytes(args[2]);
                string name = Path.GetFileName(args[2]);
                string versionText = name["Techmo-Bloodborne-".Length..^"-win-x64.zip".Length];
                var version = Version.Parse(versionText); version = new Version(version.Major,version.Minor,Math.Max(version.Build,0));
                var release = new TechmoRelease(version,"v"+versionText,"",name,
                    $"https://github.com/{TechmoUpdates.Repository}/releases/download/v{versionText}/{name}",bytes.Length,Hash(bytes),null);
                using var http = new HttpClient(new Network(bytes));
                var full = await new TechmoUpdates(http).PrepareAsync(release,NewRoot(),null,default);
                Check(full.Files.Count > 100, "The real portable ZIP passes package verification");
                Check(await Install(full)==0, "The real portable ZIP installs through the production installer");
                Check(FileVersionInfo.GetVersionInfo(Path.Combine(full.Root,"builds/launcher/Techmo.Launcher.exe")).FileVersion == version.ToString(3)+".0", "Installed executable has the package version");
                Console.WriteLine("PASS: real package validation and installation: "+name);
            }
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); Console.Error.WriteLine("Fixtures: "+fixture); return 1; }
    }
}
