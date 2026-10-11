using System.IO;
using System.Text.Json;

namespace BloodborneLauncher;

// File readiness only. The renderer checks device features and NGX at runtime.
internal static class UpscalerAssets
{
    internal static bool DlssInstalled(string path)
    {
        try
        {
            using var file = File.OpenRead(path);
            using var reader = new BinaryReader(file);
            if (file.Length < 64 || reader.ReadUInt16() != 0x5A4D) return false;
            file.Position = 0x3C;
            uint offset = reader.ReadUInt32();
            if (offset > file.Length - 26) return false;
            file.Position = offset;
            if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664) return false;
            file.Position = offset + 22;
            if ((reader.ReadUInt16() & 0x2000) == 0) return false; // IMAGE_FILE_DLL
            return reader.ReadUInt16() == 0x20B; // PE32+
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    internal static bool Fsr4Installed(string directory)
    {
        try
        {
            if (!Spirv(Path.Combine(directory, "rcas.spv")) ||
                !Spirv(Path.Combine(directory, "spd_auto_exposure.spv"))) return false;
            foreach (string model in new[] { "native", "quality", "balanced", "performance", "ultraperf", "drs" })
            {
                string prefix = Path.Combine(directory, $"fsr4_model_v07_i8_{model}");
                if (!Nonempty(prefix + "_initializers.bin") || !Nonempty(prefix + "_pre_weights.bin")) return false;
                using var manifest = JsonDocument.Parse(File.ReadAllText(prefix + "_shader_manifest.json"));
                if (!manifest.RootElement.TryGetProperty("artifacts", out var artifacts) ||
                    artifacts.ValueKind != JsonValueKind.Object) return false;
                foreach (string tier in new[] { "1080", "2160" })
                {
                    if (!Spirv(prefix + $"_{tier}_pre.spv") || !Spirv(prefix + $"_{tier}_post.spv")) return false;
                    for (int pass = 1; pass <= 12; pass++)
                        if (!Spirv(prefix + $"_{tier}_pass{pass}.spv")) return false;
                }
            }
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return false; }
    }

    static bool Nonempty(string path) => new FileInfo(path) is { Exists: true, Length: > 0 };

    static bool Spirv(string path)
    {
        using var file = File.OpenRead(path);
        using var reader = new BinaryReader(file);
        return file.Length >= 20 && file.Length % 4 == 0 && reader.ReadUInt32() == 0x07230203;
    }
}
