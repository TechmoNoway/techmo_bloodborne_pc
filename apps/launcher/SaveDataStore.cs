// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace BloodborneLauncher;

internal sealed record SaveDataSet(string UserId, string TitleId, int FileCount, long Bytes, DateTime ModifiedUtc)
{
    public string Label => $"{TitleId} · {L10n.Text("User")} {UserId} · {Bytes / 1048576.0:0.0} MB";
    public override string ToString() => Label;
}
internal sealed record SaveBackup(string Path, string UserId, string TitleId, DateTime CreatedUtc, int FileCount, long Bytes)
{
    public string Label => $"{CreatedUtc.ToLocalTime():yyyy-MM-dd HH:mm:ss} · {TitleId} · {L10n.Text("User")} {UserId} · {Bytes / 1048576.0:0.0} MB";
    public override string ToString() => Label;
}

internal sealed class SaveDataStore
{
    public string SaveRoot { get; }
    public string BackupRoot { get; }
    readonly string userRoot;
    readonly Func<bool> gameRunning;
    const int MaxFiles = 4096;
    const long MaxBytes = 2L * 1024 * 1024 * 1024;
    static readonly Regex UserPattern = new("^[0-9]{1,10}$", RegexOptions.CultureInvariant);
    static readonly Regex TitlePattern = new("^CUSA[0-9]{5}$", RegexOptions.CultureInvariant);
    sealed class Manifest
    {
        public int Version { get; set; } = 1;
        public string UserId { get; set; } = "";
        public string TitleId { get; set; } = "";
        public DateTime CreatedUtc { get; set; }
        public List<SaveFile> Files { get; set; } = new();
    }
    sealed record SaveFile(string Name, long Bytes, string Sha256);

    public SaveDataStore(string userDirectory, Func<bool> isGameRunning)
    {
        userRoot = Path.GetFullPath(userDirectory);
        SaveRoot = Path.Combine(userRoot, "savedata");
        BackupRoot = Path.Combine(userRoot, "save-backups");
        gameRunning = isGameRunning;
    }

    public List<SaveDataSet> ListSaves()
    {
        var result = new List<SaveDataSet>();
        if (!Directory.Exists(SaveRoot)) return result;
        AssertNoLinks(SaveRoot);
        foreach (string user in Directory.EnumerateDirectories(SaveRoot))
        {
            string userId = Path.GetFileName(user);
            if (!UserPattern.IsMatch(userId)) continue;
            AssertNoLinks(user);
            foreach (string title in Directory.EnumerateDirectories(user))
            {
                string titleId = Path.GetFileName(title);
                if (!TitlePattern.IsMatch(titleId)) continue;
                var files = FilesFor(userId, titleId).ToArray();
                if (files.Length > 0) result.Add(new(userId, titleId, files.Length, files.Sum(f => f.Length), files.Max(f => f.LastWriteTimeUtc)));
            }
        }
        return result.OrderByDescending(s => s.ModifiedUtc).ToList();
    }

    public List<SaveBackup> ListBackups()
    {
        if (!Directory.Exists(BackupRoot)) return new();
        AssertNoLinks(BackupRoot);
        var result = new List<SaveBackup>();
        foreach (string path in Directory.EnumerateFiles(BackupRoot, "*.zip"))
        {
            try { result.Add(Inspect(path)); }
            catch (Exception error) when (error is IOException or InvalidDataException or JsonException) { }
        }
        return result.OrderByDescending(b => b.CreatedUtc).ToList();
    }

    public SaveBackup Inspect(string path)
    {
        AssertNoLinks(path);
        using var archive = ZipFile.OpenRead(path);
        Manifest manifest = ReadManifest(archive);
        return Summary(path, manifest);
    }

    public SaveBackup Backup(string userId, string titleId)
    {
        using var operation = Lock();
        return BackupCore(userId, titleId);
    }

    SaveBackup BackupCore(string userId, string titleId)
    {
        ValidateIds(userId, titleId); EnsureStopped();
        var files = FilesFor(userId, titleId).ToArray();
        if (files.Length is < 1 or > MaxFiles || files.Sum(f => f.Length) > MaxBytes)
            throw new InvalidDataException("No usable save data, or the backup limit was exceeded.");
        Directory.CreateDirectory(BackupRoot); AssertNoLinks(BackupRoot);
        string path = Path.Combine(BackupRoot, $"{titleId}-user{userId}-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.zip");
        string temporary = path + ".tmp";
        var manifest = new Manifest { UserId = userId, TitleId = titleId, CreatedUtc = DateTime.UtcNow };
        string accountRoot = Path.Combine(SaveRoot, userId);
        try
        {
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None))
            {
                using (var archive = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
                {
                    foreach (var file in files)
                    {
                        string name = Path.GetRelativePath(accountRoot, file.FullName).Replace('\\', '/');
                        ValidateName(name, titleId);
                        using var input = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read);
                        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                        using var entry = archive.CreateEntry("data/" + name, CompressionLevel.Fastest).Open();
                        long bytes = Copy(input, entry, hash, file.Length);
                        manifest.Files.Add(new(name, bytes, Convert.ToHexString(hash.GetHashAndReset())));
                    }
                    byte[] metadata = JsonSerializer.SerializeToUtf8Bytes(manifest);
                    if (metadata.Length > 1024 * 1024) throw new InvalidDataException("Invalid backup contents or size.");
                    using var writer = archive.CreateEntry("manifest.json", CompressionLevel.Fastest).Open();
                    writer.Write(metadata);
                }
                output.Flush(flushToDisk: true);
            }
            EnsureStopped();
            File.Move(temporary, path);
            return Summary(path, manifest);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    public string? Restore(string archivePath)
    {
        using var operation = Lock();
        AssertNoLinks(archivePath);
        using var archive = ZipFile.OpenRead(archivePath);
        Manifest manifest = ReadManifest(archive);
        Directory.CreateDirectory(SaveRoot); AssertNoLinks(SaveRoot);
        string stage = Path.Combine(SaveRoot, ".techmo-restore-" + Guid.NewGuid().ToString("N"));
        string rollback = Path.Combine(SaveRoot, ".techmo-rollback-" + Guid.NewGuid().ToString("N"));
        string account = Path.Combine(SaveRoot, manifest.UserId);
        string[] roots = { manifest.TitleId, manifest.TitleId + ".memory" };
        var moved = new List<string>(); var installed = new List<string>();
        bool committed = false, recoveryFailed = false;
        string? safetyBackup = null;
        try
        {
            Directory.CreateDirectory(stage);
            foreach (var file in manifest.Files)
            {
                string destination = Contained(stage, file.Name.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                using var source = archive.GetEntry("data/" + file.Name)!.Open();
                using var target = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                Copy(source, target, hash, file.Bytes);
                target.Flush(flushToDisk: true);
                if (!Convert.ToHexString(hash.GetHashAndReset()).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The backup failed its integrity check.");
            }
            EnsureStopped();
            // Validate all live paths before making a safety backup or moving any directory.
            var existing = FilesFor(manifest.UserId, manifest.TitleId).ToArray();
            if (existing.Length > 0)
                safetyBackup = BackupCore(manifest.UserId, manifest.TitleId).Path;
            EnsureStopped(); Directory.CreateDirectory(account); AssertNoLinks(account);
            Directory.CreateDirectory(rollback);
            foreach (string name in roots)
            {
                string current = Contained(account, name);
                if (Directory.Exists(current)) { Directory.Move(current, Contained(rollback, name)); moved.Add(name); }
                if (Directory.Exists(Contained(stage, name))) { Directory.Move(Contained(stage, name), current); installed.Add(name); }
            }
            committed = true;
            return safetyBackup;
        }
        catch
        {
            try
            {
                foreach (string name in installed.AsEnumerable().Reverse()) Directory.Move(Contained(account, name), Contained(stage, name));
                foreach (string name in moved.AsEnumerable().Reverse()) Directory.Move(Contained(rollback, name), Contained(account, name));
            }
            catch { recoveryFailed = true; throw new IOException("Restore rollback needs manual recovery: " + rollback); }
            throw;
        }
        finally
        {
            if (!recoveryFailed)
            {
                RemoveTemporary(stage);
                if (committed || moved.Count == 0) RemoveTemporary(rollback);
                else if (Directory.Exists(rollback) && !Directory.EnumerateFileSystemEntries(rollback).Any()) RemoveTemporary(rollback);
            }
        }
    }

    FileStream Lock()
    {
        EnsureStopped(); Directory.CreateDirectory(userRoot); AssertNoLinks(userRoot);
        return new FileStream(Path.Combine(userRoot, ".techmo-save-operation.lock"), FileMode.OpenOrCreate,
            FileAccess.ReadWrite, FileShare.None);
    }
    void EnsureStopped()
    { if (gameRunning()) throw new IOException("Close the game before backing up or restoring save data."); }
    static SaveBackup Summary(string path, Manifest m) => new(Path.GetFullPath(path), m.UserId, m.TitleId, m.CreatedUtc, m.Files.Count, m.Files.Sum(f => f.Bytes));
    IEnumerable<FileInfo> FilesFor(string userId, string titleId)
    {
        ValidateIds(userId, titleId);
        string account = Contained(SaveRoot, userId); AssertNoLinks(SaveRoot); AssertNoLinks(account);
        foreach (string directory in new[] { titleId, titleId + ".memory" })
        {
            string path = Contained(account, directory);
            if (Directory.Exists(path)) foreach (var file in Walk(path)) yield return file;
        }
    }
    static IEnumerable<FileInfo> Walk(string directory)
    {
        AssertNoLinks(directory);
        foreach (string file in Directory.EnumerateFiles(directory)) { AssertNoLinks(file); yield return new FileInfo(file); }
        foreach (string child in Directory.EnumerateDirectories(directory)) foreach (var file in Walk(child)) yield return file;
    }
    static Manifest ReadManifest(ZipArchive archive)
    {
        var entry = archive.GetEntry("manifest.json");
        if (entry is null || entry.Length > 1024 * 1024 || archive.Entries.Count > MaxFiles + 1)
            throw new InvalidDataException("This is not a supported Techmo save backup.");
        using var stream = entry.Open();
        var manifest = JsonSerializer.Deserialize<Manifest>(stream) ?? throw new InvalidDataException("Invalid backup manifest.");
        ValidateIds(manifest.UserId, manifest.TitleId);
        if (manifest.Version != 1 || manifest.Files is null || manifest.Files.Count is < 1 or > MaxFiles ||
            manifest.Files.Any(f => f is null || f.Bytes < 0 || f.Bytes > MaxBytes || f.Sha256 is null || !Regex.IsMatch(f.Sha256, "^[A-Fa-f0-9]{64}$")) ||
            manifest.Files.Sum(f => f.Bytes) > MaxBytes || archive.Entries.Count != manifest.Files.Count + 1)
            throw new InvalidDataException("Invalid backup contents or size.");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in manifest.Files)
        {
            ValidateName(file.Name, manifest.TitleId);
            if (!names.Add(file.Name) || archive.GetEntry("data/" + file.Name) is not {} data || data.Length != file.Bytes)
                throw new InvalidDataException("Missing, duplicate or invalid backup entry.");
        }
        if (!manifest.Files.Any(f => f.Name.StartsWith(manifest.TitleId + "/", StringComparison.Ordinal)))
            throw new InvalidDataException("The backup contains no game save files.");
        if (!archive.Entries.Select(e => e.FullName).ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(names.Select(n => "data/" + n).Append("manifest.json")))
            throw new InvalidDataException("The archive contains unexpected files.");
        return manifest;
    }
    static void ValidateIds(string? user, string? title)
    { if (user is null || title is null || !UserPattern.IsMatch(user) || !TitlePattern.IsMatch(title)) throw new InvalidDataException("Invalid save user or title ID."); }
    static void ValidateName(string? name, string title)
    {
        string[] parts = name?.Split('/') ?? Array.Empty<string>();
        if (parts.Length < 2 || parts[0] != title && parts[0] != title + ".memory" ||
            parts.Any(p => p.Length == 0 || p is "." or ".." || p.EndsWith('.') || p.EndsWith(' ') || p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                Regex.IsMatch(p.Split('.')[0], "^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)))
            throw new InvalidDataException("The archive contains an unsafe path.");
    }
    static string Contained(string directory, string relative)
    {
        string prefix = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        string result = Path.GetFullPath(Path.Combine(prefix, relative));
        if (!result.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Path escapes the save directory.");
        return result;
    }
    static void AssertNoLinks(string path)
    {
        for (string? current = Path.GetFullPath(path); current is not null; current = Path.GetDirectoryName(current))
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Save operations do not follow symbolic links or junctions.");
    }
    void RemoveTemporary(string path)
    {
        _ = Contained(SaveRoot, Path.GetRelativePath(SaveRoot, path));
        if (!Path.GetFileName(path).StartsWith(".techmo-", StringComparison.Ordinal)) throw new IOException("Invalid temporary directory.");
        try { if (Directory.Exists(path)) { _ = Walk(path).ToArray(); Directory.Delete(path, recursive: true); } }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { } // Keep recoverable leftovers if Windows still holds a file.
    }
    static long Copy(Stream input, Stream output, IncrementalHash hash, long expected)
    {
        byte[] buffer = new byte[81920]; long total = 0;
        for (int count; (count = input.Read(buffer)) > 0;)
        {
            total += count; if (total > expected || total > MaxBytes) throw new InvalidDataException("Save data changed or exceeded its declared size.");
            hash.AppendData(buffer, 0, count); output.Write(buffer, 0, count);
        }
        if (total != expected) throw new InvalidDataException("Save data changed or the backup is truncated.");
        return total;
    }
}
