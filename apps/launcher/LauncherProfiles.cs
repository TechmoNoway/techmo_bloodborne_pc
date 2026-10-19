// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher;

internal sealed class LauncherProfile
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = "";
    public Dictionary<string, JsonElement> Settings { get; set; } = new();
    public Dictionary<string, string> Bindings { get; set; } = new();
    public override string ToString() => Name;
}

internal sealed class ProfileLibrary
{
    public int Version { get; set; } = 1;
    public string ActiveProfileId { get; set; } = "";
    public List<LauncherProfile> Profiles { get; set; } = new();
}

internal sealed class LauncherProfileStore(string path)
{
    internal const int MaxProfiles = 50;
    internal bool RecoveredBackup { get; private set; }
    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    internal ProfileLibrary Load(Action<LauncherProfile>? validateProfile = null)
    {
        if (!File.Exists(path)) return new();
        try { return Read(path, validateProfile); }
        catch (Exception error) when (error is JsonException or InvalidDataException)
        {
            // Preserve the damaged file before allowing the recovered library to be saved.
            var recovered = Read(path + ".bak", validateProfile);
            File.Copy(path, path + ".corrupt-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fffffff"));
            RecoveredBackup = true;
            return recovered;
        }
    }

    static ProfileLibrary Read(string source, Action<LauncherProfile>? validateProfile)
    {
        if (new FileInfo(source).Length > 2 * 1024 * 1024)
            throw new InvalidDataException("Profile library is too large.");
        var library = JsonSerializer.Deserialize<ProfileLibrary>(File.ReadAllText(source))
            ?? throw new InvalidDataException("Invalid profile library.");
        Validate(library);
        foreach (var profile in library.Profiles) validateProfile?.Invoke(profile);
        return library;
    }

    internal void Save(ProfileLibrary library)
    {
        Validate(library);
        AtomicSettingsFile.Write(path, JsonSerializer.Serialize(library, JsonOptions), keepBackup: true);
    }

    internal static string NormalizeName(string? name)
    {
        string result = name?.Trim() ?? "";
        if (result.Length is < 1 or > 60 || result.Any(char.IsControl))
            throw new InvalidDataException("Use a profile name with 1–60 characters.");
        return result;
    }

    static void Validate(ProfileLibrary library)
    {
        if (library.Version != 1) throw new NotSupportedException("This profile library was created by an unsupported launcher version.");
        if (library.Profiles is null || library.Profiles.Count is < 1 or > MaxProfiles)
            throw new InvalidDataException("Unsupported or invalid profile library.");
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var profile in library.Profiles)
        {
            if (profile is null || !Guid.TryParseExact(profile.Id, "N", out _) || !ids.Add(profile.Id) ||
                !names.Add(NormalizeName(profile.Name)) || profile.Settings is null || profile.Bindings is null ||
                profile.Settings.Count > 64 || profile.Bindings.Count > 64)
                throw new InvalidDataException("Invalid or duplicate profile.");
        }
        if (!ids.Contains(library.ActiveProfileId)) throw new InvalidDataException("Active profile is missing.");
    }
}

internal static class AtomicSettingsFile
{
    internal static void Write(string path, string content, bool keepBackup = false)
    {
        string absolute = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(absolute)!);
        string temporary = absolute + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                4096, FileOptions.WriteThrough))
            {
                byte[] bytes = new UTF8Encoding(false).GetBytes(content);
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            if (File.Exists(absolute)) File.Replace(temporary, absolute, keepBackup ? absolute + ".bak" : null);
            else File.Move(temporary, absolute);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
