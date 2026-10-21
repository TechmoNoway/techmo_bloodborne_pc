using Microsoft.Win32;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Imaging;

namespace BloodborneLauncher;

public partial class MainWindow : Window
{
    sealed class LauncherState
    {
        public string GamePath { get; set; } = "";
        public string Resolution { get; set; } = "1920x1080";
        public bool Fullscreen { get; set; } = true;
        public bool ShowFps { get; set; }
        public bool ShowCpu { get; set; }
        public bool ShowGpu { get; set; }
        public string PresentMode { get; set; } = "";
        // Physical-display calibration is shared by all gameplay profiles.
        public bool HdrOutput { get; set; }
        public string HdrWhite { get; set; } = "203";
        public string HdrPeak { get; set; } = "1000";
        public bool HdrExpand { get; set; }
        public string Upscaler { get; set; } = "fsr3";
        public string Preset { get; set; } = "1";
        public string Fps { get; set; } = "60";
        public string Language { get; set; } = "1";
        public string UiLanguage { get; set; } = "en";
        public string UiTheme { get; set; } = "dark";
        public bool ReducedMotion { get; set; }
        public string LiveResolution { get; set; } = "0";
        public string ModelLod { get; set; } = "0";
        public bool Sharpen { get; set; } = true;
        public double Sharpness { get; set; } = 0.3;
        public bool Chromatic { get; set; } = true;
        public bool Dof { get; set; } = true;
        public bool MotionBlur { get; set; } = true;
        public bool Ssao { get; set; } = true;
        public bool GameAa { get; set; } = true;
        public bool Shadows { get; set; } = true;
        public bool Ssr { get; set; }
        public bool PcControls {get;set;}=true;
        public bool Widescreen {get;set;}=true;
        public bool MouseCamera {get;set;}=true;
        public bool MouseMenu {get;set;}=true;
        public bool MouseInvertX {get;set;}
        public bool MouseInvertY {get;set;}
        public bool MouseAutoRotation {get;set;}
        public int MouseSensitivity {get;set;}=5;
        public Dictionary<string,string>? CustomInputBindings {get;set;}
        public bool CameraControls { get; set; }
        public string CameraFov { get; set; } = "1.00";
        public string CameraDistance { get; set; } = "1.00";
        public string CameraHeight { get; set; } = "1.00";
        public bool ChangeAppearance { get; set; }
        public bool Rebirth { get; set; }
        // Derived from individual lighting/color overrides; retained for v1 profile compatibility.
        public bool GraphicsControls { get; set; }
        public bool GraphicsVignette { get; set; } = true;
        public string GraphicsAo { get; set; } = "1.00";
        public string GraphicsShadows { get; set; } = "1.00";
        public string GraphicsBloom { get; set; } = "1.00";
        public string GraphicsSaturation { get; set; } = "1.00";
        public bool SkipIntro { get; set; }
        public int GamepadIndex { get; set; }
        public bool ModsEnabled { get; set; } = true;
        public bool DeveloperMode { get; set; }
        public bool DetailedLogs { get; set; }
        public bool FrameStats { get; set; }
        public bool GpuProfile { get; set; }
        public bool AudioStats { get; set; }
        public bool FsrProfile { get; set; }
        public bool OnlineEnabled { get; set; }
        public bool OnlineConnected { get; set; } = true;
        public bool OnlineUpnp { get; set; }
        public bool OnlineRemember { get; set; }
        public string OnlineHost { get; set; } = "";
        public string OnlinePort { get; set; } = "31313";
        public string OnlineP2pPort { get; set; } = "";
        public string OnlineWebApi { get; set; } = "";
        public string OnlineGameApi { get; set; } = "";
        public string OnlineWebsite { get; set; } = "";
        public string OnlineUsername { get; set; } = "";
    }

    readonly string root;
    readonly string statePath;
    LauncherState state = new();
    bool syncing;
    bool gameRunning;

    public MainWindow()
    {
        InitializeComponent();
        root = FindProjectRoot();
        statePath = Path.Combine(root, "launcher-settings.json");
        ModsFolderText.Text = Path.Combine(root, "mods");
        ConfigureCameraChoices();
        LoadState();
        ApplyStateToUi();
        InitializePresentation();
        LoadHeroImage();
        RefreshStatus();
        LoadLatestSession();
        InitializeHub();
        InitializeProfiles();
        InitializeSaves();
        InitializeMods();
        InitializeUndo();
        InitializeLanternOptions();
    }

    static string FindProjectRoot()
    {
        if (Environment.GetEnvironmentVariable("TECHMO_EXPERIMENT_ROOT") is { Length: > 0 } selected &&
            File.Exists(Path.Combine(selected, "run.bat")) && File.Exists(Path.Combine(selected, "build.sh")))
            return Path.GetFullPath(selected);
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir != null; ++i, dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "run.bat")) &&
                File.Exists(Path.Combine(dir.FullName, "build.sh")))
                return dir.FullName;
        }
        return AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    }

    void LoadState()
    {
        try
        {
            if (File.Exists(statePath))
                state = JsonSerializer.Deserialize<LauncherState>(File.ReadAllText(statePath)) ?? new();
            else
            {
                string remembered = Path.Combine(root, "out", "game_dir.txt");
                if (File.Exists(remembered)) state.GamePath = File.ReadAllText(remembered).Trim();
            }
        }
        catch { state = new(); }
        // Migrate retired or malformed frame-rate selections before displaying or launching.
        state.Fps = NormalizeFps(state.Fps);
        state.UiLanguage = Localization.Normalize(state.UiLanguage);
        if (ReadIniValue("show_fps") is string showFps) state.ShowFps = showFps == "1";
        if ((ReadIniValue("show_fps_cpu") ?? ReadIniValue("show_cpu")) is string showCpu) state.ShowCpu = showCpu == "1";
        if ((ReadIniValue("show_fps_gpu") ?? ReadIniValue("show_gpu")) is string showGpu) state.ShowGpu = showGpu == "1";
        if (ReadIniValue("sharpen") is string sharpen) state.Sharpen = sharpen == "1";
        state.Sharpness = double.TryParse(ReadIniValue("sharpness"), NumberStyles.Float, CultureInfo.InvariantCulture, out double sharpness)
            ? NormalizeSharpness(sharpness) : NormalizeSharpness(state.Sharpness);
        string? mirror = ReadIniValue("change_appearance");
        if (mirror != null) state.ChangeAppearance = mirror == "1";
        string? rebirth = ReadIniValue("rebirth");
        if (rebirth != null) state.Rebirth = rebirth == "1";
        if (ReadIniValue("graphics_vignette") is string vignette) state.GraphicsVignette = vignette == "1";
        state.GraphicsAo = GraphicsScale(ReadIniValue("graphics_ao_strength") ?? state.GraphicsAo, 0, 2);
        state.GraphicsShadows = GraphicsScale(ReadIniValue("graphics_shadow_scale") ?? state.GraphicsShadows, 1, 3);
        state.GraphicsBloom = GraphicsScale(ReadIniValue("graphics_bloom") ?? state.GraphicsBloom, 0, 1);
        state.GraphicsSaturation = GraphicsScale(ReadIniValue("graphics_saturation") ?? state.GraphicsSaturation, 0, 2);
        state.GraphicsControls = HasCustomLighting(state);
        // Native pages and the legacy overlay use this same configuration.
        if (ReadIniValue("output_res") is string output) state.Resolution = output;
        if (ReadIniValue("effect_game_aa") is string aa) state.GameAa = aa == "1";
        if (ReadIniValue("effect_ssao") is string ssao) state.Ssao = ssao == "1";
        if (ReadIniValue("effect_dof") is string dof) state.Dof = dof == "1";
        if (ReadIniValue("effect_motion_blur") is string blur) state.MotionBlur = blur == "1";
        if (ReadIniValue("effect_chromatic_aberration") is string ca) state.Chromatic = ca == "1";
        string? camera = ReadIniValue("camera_controls");
        if (camera != null) state.CameraControls = camera == "1";
        state.CameraFov = CameraScale(ReadIniValue("camera_fov_scale") ?? state.CameraFov, 1);
        state.CameraDistance = CameraScale(ReadIniValue("camera_distance_scale") ?? state.CameraDistance, 0.5);
        state.CameraHeight = CameraScale(ReadIniValue("camera_height_scale") ?? state.CameraHeight, 0.5);
        LoadInputState();
        LoadOnlineState();
        LoadModSettings();
    }

    static string NormalizeFps(string? value) => value is "30" or "60" or "90" or "120" or "144" or "uncap" ? value : "60";
    static string FpsLabel(string value) => value == "uncap" ? Localization.Current["Ui.Unlimited"] : value + " FPS";

    static string CameraScale(string? value, double min)
    {
        if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double scale) || !double.IsFinite(scale)) scale = 1;
        return Math.Clamp(scale, min, 1.5).ToString("0.00", CultureInfo.InvariantCulture);
    }

    void ConfigureCameraChoices()
    {
        foreach (var (box, min, step) in new[] { (CameraFovCombo, 1.0, 0.05), (CameraDistanceCombo, 0.5, 0.1), (CameraHeightCombo, 0.5, 0.1) })
            for (int i = 0; i <= 10; ++i)
            {
                double value = min + step * i;
                box.Items.Add(new ComboBoxItem { Tag = value.ToString("0.00", CultureInfo.InvariantCulture), Content = $"{value * 100:0}%" });
            }
    }

    static string GraphicsScale(string? text, double min, double max)
    {
        if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double value) || !double.IsFinite(value)) value = 1;
        return Math.Clamp(value, min, max).ToString("0.00", CultureInfo.InvariantCulture);
    }
    static void SetCameraScale(ComboBox box, string value)
    {
        // Preserve an intermediate scale saved by the in-game slider.
        if (!box.Items.Cast<ComboBoxItem>().Any(item => item.Tag?.ToString() == value))
            box.Items.Add(new ComboBoxItem { Tag = value, Content = $"{double.Parse(value, CultureInfo.InvariantCulture) * 100:0}%" });
        SetComboTag(box, value);
    }

    string? ReadIniValue(string key)
    {
        string path = Path.Combine(root, "bbport.ini");
        if (!File.Exists(path)) return null;
        foreach (string line in File.ReadLines(path))
        {
            int equals = line.IndexOf('=');
            if (equals > 0 && line[..equals].Trim() == key)
            {
                string value = line[(equals + 1)..].Trim();
                return key.StartsWith("bind.", StringComparison.Ordinal) && File.Exists(Path.Combine(root, "engine.lock.json"))
                    ? SwapLanternMouseTokens(value) : value;
            }
        }
        return null;
    }

    void ApplyStateToUi()
    {
        syncing = true;
        Localization.Current.SetLanguage(state.UiLanguage);
        PathBox.Text = state.GamePath;
        SetComboContent(ResolutionCombo, state.Resolution);
        SetComboTag(PresentCombo, state.PresentMode);
        UpdatePresentModeDescription();
        FullscreenCheck.IsChecked = state.Fullscreen;
        ShowFpsCheck.IsChecked = state.ShowFps;
        ShowCpuCheck.IsChecked = state.ShowCpu;
        ShowGpuCheck.IsChecked = state.ShowGpu;
        HdrOutputCheck.IsChecked = state.HdrOutput;
        SetComboTag(HdrWhiteCombo, state.HdrWhite);
        SetComboTag(HdrPeakCombo, state.HdrPeak);
        HdrExpandCheck.IsChecked = state.HdrExpand;
        UpdateHdrControls();

        SetComboTag(UpscalerCombo, state.Upscaler);
        SetComboTag(PresetCombo, state.Preset);
        SetComboTag(FpsCombo, state.Fps);

        SetComboTag(UiLanguageCombo, state.UiLanguage);
        SetComboTag(ThemeCombo, state.UiTheme);
        ReducedMotionCheck.IsChecked = state.ReducedMotion;
        SetComboTag(LiveResolutionCombo, state.LiveResolution);
        SetComboTag(ModelLodCombo, state.ModelLod);
        SharpenCheck.IsChecked = state.Sharpen;
        SharpnessSlider.Value = state.Sharpness;
        UpdateSharpnessUi();
        ChromaticCheck.IsChecked = state.Chromatic;
        DofCheck.IsChecked = state.Dof;
        MotionBlurCheck.IsChecked = state.MotionBlur;
        SsaoCheck.IsChecked = state.Ssao;
        GameAaCheck.IsChecked = state.GameAa;
        ShadowsCheck.IsChecked = state.Shadows;
        SsrCheck.IsChecked = state.Ssr;
        SkipIntroCheck.IsChecked = state.SkipIntro;
        ChangeAppearanceCheck.IsChecked = state.ChangeAppearance;
        RebirthCheck.IsChecked = state.Rebirth;
        ApplyInputUi();
        ApplyLightingUi();
        CameraControlsCheck.IsChecked = state.CameraControls;
        SetCameraScale(CameraFovCombo, state.CameraFov);
        SetCameraScale(CameraDistanceCombo, state.CameraDistance);
        SetCameraScale(CameraHeightCombo, state.CameraHeight);

        ControllerCombo.SelectedIndex = Math.Clamp(state.GamepadIndex, 0, 3);
        ModsEnabledCheck.IsChecked = state.ModsEnabled;
        DeveloperModeCheck.IsChecked = state.DeveloperMode;
        DetailedLogsCheck.IsChecked = state.DetailedLogs;
        FrameStatsCheck.IsChecked = state.FrameStats;
        GpuProfileCheck.IsChecked = state.GpuProfile;
        AudioStatsCheck.IsChecked = state.AudioStats;
        FsrProfileCheck.IsChecked = state.FsrProfile;
        UpdateUpscalingModeAvailability();
        ApplyOnlineUi();
        ApplyAppearance();
        UpdateHubSummary();
        syncing = false;
    }

    void CollectUi()
    {
        state.GamePath = PathBox.Text.Trim();
        state.Resolution = ComboText(ResolutionCombo, "1920x1080");
        state.Fullscreen = FullscreenCheck.IsChecked == true;
        state.ShowFps = ShowFpsCheck.IsChecked == true;
        state.ShowCpu = ShowCpuCheck.IsChecked == true;
        state.ShowGpu = ShowGpuCheck.IsChecked == true;
        state.PresentMode = ComboTag(PresentCombo);
        state.HdrOutput = HdrOutputCheck.IsChecked == true;
        state.HdrWhite = ComboTag(HdrWhiteCombo, "203");
        state.HdrPeak = ComboTag(HdrPeakCombo, "1000");
        state.HdrExpand = HdrExpandCheck.IsChecked == true;
        state.Upscaler = ComboTag(UpscalerCombo, "fsr3");
        state.Preset = ComboTag(PresetCombo, "1");
        state.Fps = NormalizeFps(ComboTag(FpsCombo, "60"));
        state.UiLanguage = Localization.Normalize(ComboTag(UiLanguageCombo, "en"));
        state.UiTheme = ComboTag(ThemeCombo, "dark");
        state.ReducedMotion = ReducedMotionCheck.IsChecked == true;
        state.LiveResolution = ComboTag(LiveResolutionCombo, "0");
        state.ModelLod = ComboTag(ModelLodCombo, "0");
        state.Sharpen = SharpenCheck.IsChecked == true;
        state.Sharpness = NormalizeSharpness(SharpnessSlider.Value);
        state.Chromatic = ChromaticCheck.IsChecked == true;
        state.Dof = DofCheck.IsChecked == true;
        state.MotionBlur = MotionBlurCheck.IsChecked == true;
        state.Ssao = SsaoCheck.IsChecked == true;
        state.GameAa = GameAaCheck.IsChecked == true;
        state.Shadows = ShadowsCheck.IsChecked == true;
        state.Ssr = SsrCheck.IsChecked == true;
        state.SkipIntro = SkipIntroCheck.IsChecked == true;
        state.ChangeAppearance = ChangeAppearanceCheck.IsChecked == true;
        state.Rebirth = RebirthCheck.IsChecked == true;
        CollectLightingUi();
        state.CameraControls = CameraControlsCheck.IsChecked == true;
        state.CameraFov = CameraScale(ComboTag(CameraFovCombo), 1);
        state.CameraDistance = CameraScale(ComboTag(CameraDistanceCombo), 0.5);
        state.CameraHeight = CameraScale(ComboTag(CameraHeightCombo), 0.5);
        state.GamepadIndex = Math.Max(0, ControllerCombo.SelectedIndex);
        state.ModsEnabled = ModsEnabledCheck.IsChecked == true;
        state.DeveloperMode = DeveloperModeCheck.IsChecked == true;
        state.DetailedLogs = DetailedLogsCheck.IsChecked == true;
        state.FrameStats = FrameStatsCheck.IsChecked == true;
        state.GpuProfile = GpuProfileCheck.IsChecked == true;
        state.AudioStats = AudioStatsCheck.IsChecked == true;
        state.FsrProfile = FsrProfileCheck.IsChecked == true;
        CollectInputUi();
        CollectOnlineUi();
    }

    bool SaveSettings()
    {
        try
        {
        CollectUi();
        if (!profilesUnavailable && activeProfile is not null) ValidateProfile(CaptureProfile(activeProfile.Name, activeProfile.Id));
        ValidateModSettings();
        SaveLanternOptions();
        Directory.CreateDirectory(Path.Combine(root, "out"));
        AtomicSettingsFile.Write(statePath, JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }));
        if (!string.IsNullOrWhiteSpace(state.GamePath))
            File.WriteAllText(Path.Combine(root, "out", "game_dir.txt"), state.GamePath);

        var ini = new Dictionary<string, string>
        {
            ["upscaler"] = state.Upscaler,
            ["preset"] = state.Preset,
            ["sharpen"] = state.Sharpen ? "1" : "0",
            ["sharpness"] = state.Sharpness.ToString("0.00", CultureInfo.InvariantCulture),
            ["show_fps"] = state.ShowFps ? "1" : "0",
            ["show_cpu"] = state.ShowCpu ? "1" : "0",
            ["show_gpu"] = state.ShowGpu ? "1" : "0",
            ["show_fps_cpu"] = state.ShowCpu ? "1" : "0",
            ["show_fps_gpu"] = state.ShowGpu ? "1" : "0",
            ["output_res"] = state.Resolution,
            ["fullscreen"] = state.Fullscreen ? "1" : "0",
            ["live_resolution"] = state.LiveResolution,
            ["model_lod"] = state.ModelLod,
            ["effect_chromatic_aberration"] = state.Chromatic ? "1" : "0",
            ["effect_dof"] = state.Dof ? "1" : "0",
            ["effect_motion_blur"] = state.MotionBlur ? "1" : "0",
            ["effect_ssao"] = state.Ssao ? "1" : "0",
            ["effect_game_aa"] = state.GameAa ? "1" : "0",
            ["effect_dynamic_shadows"] = state.Shadows ? "1" : "0",
            ["effect_ssr"] = state.Ssr ? "1" : "0",
            ["skip_intro"] = state.SkipIntro ? "1" : "0",
            ["change_appearance"] = state.ChangeAppearance ? "1" : "0",
            ["rebirth"] = state.Rebirth ? "1" : "0",
            ["graphics_controls"] = state.GraphicsControls ? "1" : "0",
            ["graphics_vignette"] = state.GraphicsVignette ? "1" : "0",
            ["graphics_ao_strength"] = state.GraphicsAo,
            ["graphics_shadow_scale"] = state.GraphicsShadows,
            ["graphics_bloom"] = state.GraphicsBloom,
            ["graphics_saturation"] = state.GraphicsSaturation,
            ["camera_controls"] = state.CameraControls ? "1" : "0",
            ["camera_fov_scale"] = state.CameraFov,
            ["camera_distance_scale"] = state.CameraDistance,
            ["camera_height_scale"] = state.CameraHeight
        };
        SaveInputSettings(ini);
        RewriteIni(Path.Combine(root, "bbport.ini"), ini);
        if (!lanternOptionsReady) SaveOnlineSettings();
        SaveActiveProfile();
        SaveModSettings();
        SyncLanternSettings();
        RememberAppliedSettings();
        hasUnsavedChanges = false;
        UpdateSaveState();
        L10n.SetText(FooterMessage, "Settings saved");
        RefreshStatus();
        return true;
        }
        catch (Exception error)
        {
            hasUnsavedChanges = true;
            UpdateSaveState();
            SetFooterMessage("Could not save settings: {0}", error.Message);
            L10n.SetText(OnlineStatus, FooterMessage.Text);
            return false;
        }
    }

    static void RewriteIni(string path, Dictionary<string, string> values)
    {
        if (File.Exists(Path.Combine(Path.GetDirectoryName(path)!, "engine.lock.json")))
            values = values.ToDictionary(pair => pair.Key, pair => pair.Key.StartsWith("bind.", StringComparison.Ordinal)
                ? SwapLanternMouseTokens(pair.Value) : pair.Value);
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
        var written = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < lines.Count; ++i)
        {
            string line = lines[i];
            int eq = line.IndexOf('=');
            if (eq <= 0 || line.TrimStart().StartsWith("#")) continue;
            string key = line[..eq].Trim();
            if (values.TryGetValue(key, out string? value))
            {
                lines[i] = key + "=" + value;
                written.Add(key);
            }
        }
        if (lines.Count == 0) lines.Add("# Techmo Bloodborne settings");
        foreach (var kv in values)
            if (!written.Contains(kv.Key)) lines.Add(kv.Key + "=" + kv.Value);
        AtomicSettingsFile.Write(path, string.Join("\r\n", lines) + "\r\n");
    }

    void RefreshStatus() => RefreshStatusCore(detectGpu: true);

    void RefreshStatusCore(bool detectGpu)
    {
        string game = PathBox.Text.Trim();
        bool eboot = File.Exists(Path.Combine(game, "eboot.bin"));
        bool sce = Directory.Exists(Path.Combine(game, "sce_sys"));
        string version = DetectGameVersion(game);
        bool valid = eboot && sce;

        L10n.SetText(GamePathStatus, valid ? game : "Select Bloodborne folder");
        L10n.SetText(GameVersionStatus, valid ? version : "Not configured");
        GameVersionStatus.Foreground = valid && version.Contains("1.09") ?
            new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(117, 207, 121)) :
            new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(210, 107, 112));
        L10n.SetText(PathValidationText, valid ? "Game dump detected · {0}" : "Choose the folder containing eboot.bin and sce_sys.", valid ? new object[] { version } : Array.Empty<object>());
        string probe = Path.Combine(root, "out", "bb-probe.exe");
        bool nativeReady = File.Exists(probe);
        PlayButton.IsEnabled = valid && nativeReady && !gameRunning && !saveDataBusy;
        L10n.SetText(PlayHint, !valid ? "Select a valid Bloodborne dump before playing." : !nativeReady ?
            "Build the native runtime before playing." : "Ready. Settings are saved automatically when Play is pressed.");
        L10n.SetText(RuntimeStatus, File.Exists(probe) ? "Ready" : "Not built");
        RuntimeStatus.Foreground = File.Exists(probe) ? Green() : Red();

        string dlss = Path.Combine(root, "out", "nvngx_dlss.dll");
        bool dlssReady = UpscalerAssets.DlssInstalled(dlss);
        L10n.SetText(DlssStatus, dlssReady ? "Model installed · driver checked at launch" :
            File.Exists(dlss) ? "Model DLL is invalid" : "Model DLL not installed");
        DlssStatus.Foreground = dlssReady ? Green() : Muted();

        bool fsr411 = Directory.Exists(Path.Combine(root, "fsr4_411")) ||
            UpscalerAssets.DlssInstalled(Path.Combine(root, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));
        bool fsr4 = UpscalerAssets.Fsr4Installed(Path.Combine(root, "fsr4_shaders"));
        L10n.SetText(Fsr4Status, fsr411 ? "4.1.1 available" : fsr4 ? "v07 installed · experimental" :
            Directory.Exists(Path.Combine(root, "fsr4_shaders")) ? "Assets incomplete or damaged" : "Assets not installed");
        Fsr4Status.Foreground = (fsr411 || fsr4) ? Green() : Muted();

        if (detectGpu) DetectGpu();
        FooterFps.Text = "   |   " + FpsLabel(state.Fps);
    }

    static System.Windows.Media.Brush Green() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(117, 207, 121));
    static System.Windows.Media.Brush Red() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(210, 107, 112));
    static System.Windows.Media.Brush Muted() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(142, 150, 158));

    static string DetectGameVersion(string game)
    {
        try
        {
            string sfo = Path.Combine(game, "sce_sys", "param.sfo");
            if (!File.Exists(sfo)) return L10n.Text("Dump found · version unknown");
            string text = Encoding.UTF8.GetString(File.ReadAllBytes(sfo));
            if (text.Contains("01.09", StringComparison.Ordinal)) return "1.09 ✓";
            if (text.Contains("01.08", StringComparison.Ordinal)) return "1.08";
            if (text.Contains("01.00", StringComparison.Ordinal)) return "1.00";
            return L10n.Text("Dump found · verify 1.09");
        }
        catch { return L10n.Text("Dump found · version unknown"); }
    }

    async void DetectGpu()
    {
        string caps = Path.Combine(root, "out", "bb-gpu-capabilities.exe");
        if (!File.Exists(caps))
        {
            L10n.SetText(GpuStatus, "Vulkan probe not built");
            L10n.SetText(FooterGpu, "   |   GPU unknown");
            return;
        }
        try
        {
            var psi = new ProcessStartInfo(caps, "--live-resolution")
            {
                WorkingDirectory = root,
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                CreateNoWindow = true
            };
            using var p = Process.Start(psi);
            if (p == null) return;
            string stderr = await p.StandardError.ReadToEndAsync();
            await p.WaitForExitAsync();
            string gpu = stderr.Split('\n')
                .Select(x => x.Trim())
                .FirstOrDefault(x => x.StartsWith("GPU:", StringComparison.OrdinalIgnoreCase)) ?? "GPU detected by Vulkan";
            if (gpu.StartsWith("GPU:")) gpu = gpu[4..].Split(',')[0].Trim();
            L10n.SetText(GpuStatus, gpu);
            L10n.SetText(FooterGpu, "   |   {0}", gpu);
        }
        catch
        {
            L10n.SetText(GpuStatus, "Vulkan available");
            L10n.SetText(FooterGpu, "   |   Vulkan GPU");
        }
    }

    void LoadHeroImage()
    {
        HeroImage.Source = new BitmapImage(new Uri($"pack://application:,,,/{typeof(MainWindow).Assembly.GetName().Name};component/Assets/hunter-background.png"));
        string game = PathBox.Text.Trim();
        string[] candidates =
        {
            Path.Combine(game, "sce_sys", "pic1.png"),
            Path.Combine(game, "sce_sys", "pic0.png"),
            Path.Combine(root, "assets", "hero.jpg"),
            Path.Combine(root, "launcher-assets", "hero.jpg"),
            Path.Combine(AppContext.BaseDirectory, "assets", "hero.jpg"),
            Path.Combine(game, "sce_sys", "icon0.png")
        };
        foreach (string path in candidates)
        {
            if (!File.Exists(path)) continue;
            try
            {
                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                using var fs = File.OpenRead(path);
                bitmap.StreamSource = fs;
                bitmap.EndInit();
                bitmap.Freeze();
                HeroImage.Source = bitmap;
                return;
            }
            catch { }
        }
    }

    void Play_Click(object sender, RoutedEventArgs e)
    {
        if(gameRunning || saveDataBusy) return;
        if (!File.Exists(Path.Combine(root, "out", "bb-probe.exe")))
        {
            SetFooterMessage("Build the native runtime before playing.");
            RefreshStatusCore(detectGpu: false);
            return;
        }
        if (!SaveSettings()) return;
        if (!lanternOptionsReady && !ValidateOnlineForLaunch()) return;
        if (!File.Exists(Path.Combine(state.GamePath, "eboot.bin")))
        {
            MessageBox.Show(L10n.Text("Select a valid Bloodborne folder first."), "Techmo Bloodborne", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        string runBat = Path.Combine(root, "run.bat");
        if (!File.Exists(runBat))
        {
            MessageBox.Show(L10n.Text("run.bat was not found next to the runtime."), "Techmo Bloodborne", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        try
        {
            string logsDir = Path.Combine(root, "logs");
            Directory.CreateDirectory(logsDir);
            string sessionDir = Path.Combine(logsDir, $"{DateTime.Now:yyyy-MM-dd_HH-mm-ss}-{Guid.NewGuid():N}");
            Directory.CreateDirectory(sessionDir);
            string logPath = Path.Combine(sessionDir, "session.log");

            var psi = new ProcessStartInfo("cmd.exe")
            {
                WorkingDirectory = root,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                Arguments = $"/c \"\"{runBat}\" --game-dir \"{state.GamePath}\"\""
            };
            psi.Environment["BB_PREBUILT"] = "1";
            psi.Environment["BB_DATA_DIR"] = root;
            psi.Environment["BB_CONFIG"] = Path.Combine(root, "bbport.ini");
            psi.Environment["BB_GAME_DIR"] = state.GamePath;
            psi.Environment["BB_FPS"] = state.Fps;
            psi.Environment["BB_FPS_LIMIT"] = state.Fps == "uncap" ? "0" : state.Fps;
            psi.Environment["BB_VBLANK_HZ"] = state.Fps == "uncap" ? "480" : state.Fps == "90" ? "90" : "60";
            psi.Environment["BB_LANGUAGE"] = state.Language;
            psi.Environment["BB_GAMEPAD_INDEX"] = state.GamepadIndex.ToString();
            psi.Environment["BB_MODS_ENABLED"] = state.ModsEnabled ? "1" : "0";
            psi.Environment["BB_MODS_DIR"] = modsDirectory;
            psi.Environment["BB_MODS_CONFIG"] = modsConfigPath;
            psi.Environment["BB_LOG_FILE"] = logPath;
            psi.Environment["BB_SESSION_DIR"] = sessionDir;
            psi.Environment["PYTHONUNBUFFERED"] = "1";
            psi.Environment["BB_PRESENT_MODE"] = string.IsNullOrEmpty(state.PresentMode) ? "Mailbox" : state.PresentMode;
            ConfigureHdrLaunch(psi);
            if (state.DeveloperMode && state.DetailedLogs)
            {
                psi.Environment["BB_DLSS_LOG"] = "1";
                psi.Environment["BB_DIRECT_IMAGE_BARRIERS"] = "1";
            }
            // Recording does not require switches which change renderer behavior.
            if (state.FrameStats) psi.Environment["BB_FRAME_STATS"] = "1";
            else psi.Environment.Remove("BB_FRAME_STATS");
            if (state.GpuProfile) psi.Environment["BB_GPU_PROFILE"] = "1";
            else psi.Environment.Remove("BB_GPU_PROFILE");
            if (state.DeveloperMode && state.AudioStats) psi.Environment["BB_AUDIO_STATS"] = "1";
            if (state.DeveloperMode && state.FsrProfile) psi.Environment["BB_FSR4_PROFILE"] = "1";
            if (!lanternOptionsReady) ConfigureOnlineLaunch(psi);
            ConfigureLanternLaunch(psi);
            var gameProcess = new Process
            {
                StartInfo = psi,
                EnableRaisingEvents = true
            };
            gameProcess.Exited += (_, _) =>
            {
                int exitCode = gameProcess.ExitCode;
                Dispatcher.BeginInvoke(() =>
                {
                    gameRunning=false;
                    Show();
                    WindowState = WindowState.Normal;
                    LoadState();
                    ApplyStateToUi();
                    RememberAppliedSettings();
                    RefreshStatusCore(detectGpu: false);
                    RefreshSaveLists();
                    Activate();
                    ShowSessionReport(sessionDir, exitCode);
                    if (exitCode != 0 && LastSessionCard.Visibility == Visibility.Visible)
                    {
                        Nav_Click(NavHome, new RoutedEventArgs());
                        LastSessionCard.BringIntoView();
                    }
                    gameProcess.Dispose();
                });
            };
            gameRunning=true;
            PlayButton.IsEnabled=false;
            if(!gameProcess.Start()) throw new InvalidOperationException(L10n.Text("Could not start the game."));
            SetFooterMessage("Started · log: {0}", Path.GetFileName(logPath));
            Hide();
        }
        catch (Exception ex)
        {
            gameRunning=false;
            RefreshStatus();
            MessageBox.Show(ex.Message, L10n.Text("Unable to start Bloodborne"), MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void Save_Click(object sender, RoutedEventArgs e) => SaveSettings();

    void BrowseGame_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = L10n.Text("Select the Bloodborne v1.09 folder") };
        if (!string.IsNullOrWhiteSpace(PathBox.Text) && Directory.Exists(PathBox.Text))
            dialog.InitialDirectory = PathBox.Text;
        if (dialog.ShowDialog() == true)
        {
            PathBox.Text = dialog.FolderName;
            state.GamePath = dialog.FolderName;
            LoadHeroImage();
            RefreshStatus();
        }
    }

    void OpenMods_Click(object sender, RoutedEventArgs e)
    {
        string mods = modsDirectory;
        try
        {
            Directory.CreateDirectory(mods);
            Process.Start(new ProcessStartInfo(mods) { UseShellExecute = true });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        { L10n.SetText(ModScanStatus, "Could not open mod folder: {0}", error.Message); }
    }

    void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        string logs = Path.Combine(root, "logs");
        Directory.CreateDirectory(logs);
        Process.Start(new ProcessStartInfo("explorer.exe", logs) { UseShellExecute = true });
    }

    void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;
        NavigateHub(button.CommandParameter?.ToString() ?? "Home");
    }

    static string ComboTag(ComboBox box, string fallback = "") =>
        (box.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? fallback;
    static string ComboText(ComboBox box, string fallback = "") =>
        (box.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? (box.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? fallback;

    static void SetComboTag(ComboBox box, string tag)
    {
        foreach (var obj in box.Items)
            if (obj is ComboBoxItem item && (item.Tag?.ToString() ?? "") == tag) { box.SelectedItem = item; return; }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }

    static void SetComboContent(ComboBox box, string value)
    {
        foreach (var obj in box.Items)
            if (obj is ComboBoxItem item && string.Equals(item.Tag?.ToString() ?? item.Content?.ToString(), value, StringComparison.OrdinalIgnoreCase)) { box.SelectedItem = item; return; }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }

    void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2) Maximize_Click(sender, e);
        else DragMove();
    }
    void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    void Maximize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    void Close_Click(object sender, RoutedEventArgs e) => Close();
}
