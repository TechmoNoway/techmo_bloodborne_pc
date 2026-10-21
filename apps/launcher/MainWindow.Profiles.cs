// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher;

public partial class MainWindow
{
    // Profiles deliberately omit installation paths, UI preferences, diagnostics and accounts.
    static readonly HashSet<string> ProfileSettingNames = new(StringComparer.Ordinal)
    {
        "Resolution", "Fullscreen", "ShowFps", "ShowCpu", "ShowGpu", "PresentMode", "Upscaler", "Preset", "Fps",
        "LiveResolution", "ModelLod", "Sharpen", "Sharpness", "Chromatic", "Dof", "MotionBlur", "Ssao",
        "GameAa", "Shadows", "Ssr", "PcControls", "Widescreen", "MouseCamera", "MouseMenu",
        "MouseInvertX", "MouseInvertY", "MouseAutoRotation", "MouseSensitivity", "CustomInputBindings",
        // GraphicsControls remains in schema v1; Apply derives it from the individual values.
        "CameraControls", "CameraFov", "CameraDistance", "CameraHeight", "GraphicsControls",
        "GraphicsVignette", "GraphicsAo", "GraphicsShadows", "GraphicsBloom", "GraphicsSaturation",
        "SkipIntro", "GamepadIndex"
    };
    LauncherProfileStore? profileStore;
    ProfileLibrary profileLibrary = new();
    LauncherProfile? activeProfile;
    bool updatingProfiles;
    bool profilesUnavailable;
    bool hasUnsavedChanges;

    void InitializeProfiles()
    {
        profileStore = new(Path.Combine(root, "user", "launcher", "profiles.json"));
        try
        {
            profileLibrary = profileStore.Load(ValidateProfile);
            if (profileLibrary.Profiles.Count == 0)
            {
                activeProfile = CaptureProfile("My setup");
                profileLibrary.Profiles.Add(activeProfile);
                profileLibrary.ActiveProfileId = activeProfile.Id;
                profileStore.Save(profileLibrary);
            }
            activeProfile = profileLibrary.Profiles.Single(p => p.Id == profileLibrary.ActiveProfileId);
            RefreshProfilePicker();
            // The runtime's current INI remains authoritative after in-game adjustments.
            var current = CaptureProfile(activeProfile.Name, activeProfile.Id);
            if (!SameProfileSettings(current, activeProfile)) MarkSettingsChanged();
            else UpdateSaveState();
            if (profileStore.RecoveredBackup)
                SetFooterMessage("Profiles recovered from backup. The damaged file was preserved.");
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or InvalidOperationException or ArgumentException or NotSupportedException)
        {
            profilesUnavailable = true;
            ProfileCombo.IsEnabled = SaveProfileCopyButton.IsEnabled = ProfileMenuButton.IsEnabled = false;
            SetFooterMessage("Profiles could not be loaded: {0}", error.Message);
        }
    }

    LauncherProfile CaptureProfile(string name, string? id = null)
    {
        CollectUi();
        var settings = JsonSerializer.SerializeToElement(state).EnumerateObject()
            .Where(p => ProfileSettingNames.Contains(p.Name)).ToDictionary(p => p.Name, p => p.Value.Clone());
        return new() { Id = id ?? Guid.NewGuid().ToString("N"), Name = name, Settings = settings,
            Bindings = keyBindings.ToDictionary(row => row.Key, row => row.Binding) };
    }

    static bool SameProfileSettings(LauncherProfile a, LauncherProfile b) =>
        a.Settings.Count == b.Settings.Count && a.Settings.All(p => b.Settings.TryGetValue(p.Key, out var value) &&
            JsonNode.DeepEquals(JsonNode.Parse(p.Value.GetRawText()), JsonNode.Parse(value.GetRawText()))) &&
        a.Bindings.Count == b.Bindings.Count && a.Bindings.All(p => b.Bindings.GetValueOrDefault(p.Key) == p.Value);

    static void ValidateProfile(LauncherProfile profile)
    {
        // These optional v1 additions default to Off in older profiles. Saving writes the
        // expanded snapshot; no existing graphics/input choices need to be discarded.
        profile.Settings.TryAdd("ShowCpu", JsonSerializer.SerializeToElement(false));
        profile.Settings.TryAdd("ShowGpu", JsonSerializer.SerializeToElement(false));
        profile.Settings.TryAdd("Sharpness", JsonSerializer.SerializeToElement(0.3));
        if (!profile.Settings.Keys.ToHashSet(StringComparer.Ordinal).SetEquals(ProfileSettingNames) ||
            profile.Bindings.Count != PcDefaults.Length || PcDefaults.Any(d => !profile.Bindings.ContainsKey(d.key)))
            throw new InvalidDataException("The profile is incomplete or contains unsupported settings.");
        var data = new JsonObject();
        foreach (var setting in profile.Settings) data[setting.Key] = JsonNode.Parse(setting.Value.GetRawText());
        var values = data.Deserialize<LauncherState>() ?? throw new InvalidDataException("Invalid profile settings.");
        if (values.Upscaler is not ("dlss" or "fsr3" or "fsr4" or "fsr411" or "taa" or "off") ||
            values.Preset is not ("0" or "1" or "2" or "3" or "4") ||
            values.Fps is not ("30" or "60" or "90" or "120" or "144" or "uncap") ||
            values.PresentMode is not ("" or "Fifo" or "FifoRelaxed" or "Immediate") ||
            values.LiveResolution is not ("0" or "auto" or "1") ||
            values.ModelLod is not ("-2" or "0" or "1" or "2") ||
            !double.IsFinite(values.Sharpness) || values.Sharpness is < 0 or > 2 ||
            values.MouseSensitivity is < 0 or > 10 || values.GamepadIndex is < 0 or > 3 ||
            !new[] { "1280x720", "1920x1080", "2560x1440", "3840x2160", "2560x1080", "3440x1440",
                "3840x1600", "3840x1080", "5120x1440", "5120x2160", "1600x900", "7680x2160",
                "1280x1024", "1680x1050", "1920x1200", "2560x1600", "auto" }.Contains(values.Resolution) ||
            CameraScale(values.CameraFov, 1) != values.CameraFov ||
            CameraScale(values.CameraDistance, .5) != values.CameraDistance ||
            CameraScale(values.CameraHeight, .5) != values.CameraHeight ||
            GraphicsScale(values.GraphicsAo, 0, 2) != values.GraphicsAo ||
            GraphicsScale(values.GraphicsShadows, 1, 3) != values.GraphicsShadows ||
            GraphicsScale(values.GraphicsBloom, 0, 1) != values.GraphicsBloom ||
            GraphicsScale(values.GraphicsSaturation, 0, 2) != values.GraphicsSaturation)
            throw new InvalidDataException("The profile contains an invalid setting.");
        if (values.CustomInputBindings is not null && !values.CustomInputBindings.Keys.ToHashSet(StringComparer.Ordinal).SetEquals(PcDefaults.Select(d => d.key)))
            throw new InvalidDataException("The profile contains an incomplete custom input preset.");
        foreach (string binding in profile.Bindings.Values.Concat(values.CustomInputBindings?.Values ?? Enumerable.Empty<string>()))
        {
            if (binding is null || binding.Length > InputKeyNames.MaxBindingLength || binding.Any(char.IsControl) ||
                binding.Split(',').Any(slot => slot.Trim() is not ("" or "none") && InputKeyNames.NormalizeChord(slot.Trim()) is null))
                throw new InvalidDataException("The profile contains an invalid key binding.");
        }
    }

    void RefreshProfilePicker()
    {
        updatingProfiles = true;
        ProfileCombo.ItemsSource = null;
        ProfileCombo.ItemsSource = profileLibrary.Profiles;
        ProfileCombo.SelectedItem = activeProfile;
        updatingProfiles = false;
        DeleteProfileMenu.IsEnabled = profileLibrary.Profiles.Count > 1;
        SaveProfileCopyButton.IsEnabled = profileLibrary.Profiles.Count < LauncherProfileStore.MaxProfiles;
        UpdateSaveState();
    }

    void MarkSettingsChanged()
    {
        if (syncing) return;
        hasUnsavedChanges = true;
        UpdateSaveState();
    }

    void UpdateSaveState()
    {
        if (ApplySettingsButton is null) return;
        ProfileDirtyDot.Visibility = hasUnsavedChanges ? Visibility.Visible : Visibility.Collapsed;
        L10n.SetText(FooterMessage, hasUnsavedChanges ? "Unsaved changes" : "Settings are up to date");
        SetApplyBarVisible(hasUnsavedChanges);
    }

    void SaveActiveProfile()
    {
        if (profilesUnavailable || activeProfile is null || profileStore is null) return;
        var snapshot = CaptureProfile(activeProfile.Name, activeProfile.Id);
        ValidateProfile(snapshot);
        var oldSettings = activeProfile.Settings;
        var oldBindings = activeProfile.Bindings;
        string oldActiveId = profileLibrary.ActiveProfileId;
        activeProfile.Settings = snapshot.Settings;
        activeProfile.Bindings = snapshot.Bindings;
        profileLibrary.ActiveProfileId = activeProfile.Id;
        try { profileStore.Save(profileLibrary); }
        catch
        {
            activeProfile.Settings = oldSettings;
            activeProfile.Bindings = oldBindings;
            profileLibrary.ActiveProfileId = oldActiveId;
            throw;
        }
    }

    void ApplyProfile(LauncherProfile profile)
    {
        ValidateProfile(profile);
        CollectUi();
        var data = JsonSerializer.SerializeToNode(state)!.AsObject();
        foreach (var setting in profile.Settings) data[setting.Key] = JsonNode.Parse(setting.Value.GetRawText());
        state = data.Deserialize<LauncherState>()!;
        foreach (var row in keyBindings) row.Binding = profile.Bindings[row.Key];
        activeProfile = profile;
        string password = OnlinePasswordBox.Password, token = OnlineTokenBox.Password;
        ApplyStateToUi();
        syncing = true;
        OnlinePasswordBox.Password = password; OnlineTokenBox.Password = token;
        syncing = false;
        SyncDisplaySummary();
        UpdateHubSummary();
        MarkSettingsChanged();
        RefreshProfilePicker();
    }

    bool ConfirmProfileSwitch(string message = "Save your changes before switching profiles?")
    {
        if (!hasUnsavedChanges) return true;
        var result = MessageBox.Show(this, L10n.Text(message),
            L10n.Text("Unsaved changes"), MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
        return result == MessageBoxResult.No || result == MessageBoxResult.Yes && SaveSettings();
    }

    void Profile_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (updatingProfiles || ProfileCombo.SelectedItem is not LauncherProfile next || next == activeProfile) return;
        if (!ConfirmProfileSwitch()) { RefreshProfilePicker(); return; }
        try { ApplyProfile(next); }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or InvalidOperationException)
        {
            RefreshProfilePicker();
            SetFooterMessage("Could not load profile: {0}", error.Message);
        }
    }

    bool CreateProfileCopy(string name)
    {
        name = LauncherProfileStore.NormalizeName(name);
        if (profileLibrary.Profiles.Count >= LauncherProfileStore.MaxProfiles ||
            profileLibrary.Profiles.Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("A profile with this name already exists, or the profile limit was reached.");
        var previous = activeProfile;
        var created = CaptureProfile(name);
        profileLibrary.Profiles.Add(created);
        activeProfile = created;
        if (!SaveSettings()) { profileLibrary.Profiles.Remove(created); activeProfile = previous; RefreshProfilePicker(); return false; }
        RefreshProfilePicker();
        return true;
    }

    void SaveProfileCopy_Click(object sender, RoutedEventArgs e)
    {
        if (AskProfileName("Save a copy", "") is string name) CreateProfileCopy(name);
    }

    void RenameProfile_Click(object sender, RoutedEventArgs e)
    {
        if (activeProfile is null || AskProfileName("Rename profile", activeProfile.Name) is not string name) return;
        string old = activeProfile.Name;
        activeProfile.Name = name;
        try { profileStore!.Save(profileLibrary); RefreshProfilePicker(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { activeProfile.Name = old; SetFooterMessage("Could not save profile: {0}", error.Message); }
    }

    void DeleteProfile_Click(object sender, RoutedEventArgs e)
    {
        if (activeProfile is null || profileLibrary.Profiles.Count < 2 || !ConfirmProfileSwitch()) return;
        if (MessageBox.Show(this, L10n.Text("Delete profile \"{0}\"? Your game saves will not be affected.", activeProfile.Name),
            L10n.Text("Delete profile"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        int index = profileLibrary.Profiles.IndexOf(activeProfile);
        var removed = activeProfile;
        string previousId = profileLibrary.ActiveProfileId;
        profileLibrary.Profiles.Remove(removed);
        var next = profileLibrary.Profiles[Math.Min(index, profileLibrary.Profiles.Count - 1)];
        profileLibrary.ActiveProfileId = next.Id;
        try { profileStore!.Save(profileLibrary); ApplyProfile(next); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            profileLibrary.Profiles.Insert(index, removed);
            profileLibrary.ActiveProfileId = previousId;
            RefreshProfilePicker();
            SetFooterMessage("Could not save profile: {0}", error.Message);
        }
    }

    void ProfileMenu_Click(object sender, RoutedEventArgs e)
    {
        ProfileMenuButton.ContextMenu.PlacementTarget = ProfileMenuButton;
        ProfileMenuButton.ContextMenu.IsOpen = true;
    }

    string? AskProfileName(string title, string initial)
    {
        var dialog = new Window { Owner = this, Title = L10n.Text(title), Width = 430, SizeToContent = SizeToContent.Height,
            ResizeMode = ResizeMode.NoResize, WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Background = (System.Windows.Media.Brush)FindResource("PopoverBrush"),
            Foreground = (System.Windows.Media.Brush)FindResource("TextBrush"), FontFamily = FontFamily, ShowInTaskbar = false };
        var panel = new StackPanel { Margin = new Thickness(24) };
        var input = new TextBox { Text = initial, MaxLength = 60, Margin = new Thickness(0, 8, 0, 12) };
        System.Windows.Automation.AutomationProperties.SetName(input, L10n.Text("Profile name"));
        panel.Children.Add(new TextBlock { Text = L10n.Text("Profile name"), FontSize = 16 });
        panel.Children.Add(input);
        panel.Children.Add(new TextBlock { Text = L10n.Text("Stores display, graphics, camera and input settings."),
            TextWrapping = TextWrapping.Wrap, Foreground = (System.Windows.Media.Brush)FindResource("MutedBrush") });
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Foreground = Red(), Margin = new Thickness(0, 8, 0, 0) };
        panel.Children.Add(error);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 20, 0, 0) };
        var cancel = new Button { Content = L10n.Text("Cancel"), IsCancel = true, Margin = new Thickness(0, 0, 8, 0) };
        var save = new Button { Content = L10n.Text(title), IsDefault = true };
        string? result = null;
        save.Click += (_, _) =>
        {
            try
            {
                string name = LauncherProfileStore.NormalizeName(input.Text);
                if (profileLibrary.Profiles.Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase) &&
                    (initial.Length == 0 || p != activeProfile)))
                    throw new InvalidDataException("A profile with this name already exists.");
                result = name;
                dialog.DialogResult = true;
            }
            catch (InvalidDataException issue) { error.Text = L10n.Text(issue.Message); }
        };
        actions.Children.Add(cancel); actions.Children.Add(save); panel.Children.Add(actions);
        dialog.Content = panel;
        dialog.Loaded += (_, _) => { input.Focus(); input.SelectAll(); };
        return dialog.ShowDialog() == true ? result : null;
    }
}
