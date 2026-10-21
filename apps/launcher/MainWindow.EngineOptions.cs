// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Media;
using N = Techmo.Engine.Core;
using V = Techmo.Engine.Views;

namespace BloodborneLauncher;

public partial class MainWindow
{
    bool engineOptionsReady, loadingEngineOptions, savingEngineOptions;
    readonly List<(string id, Button button, FrameworkElement view)> engineNavigation = new();
    readonly List<(Panel parent, UIElement content)> engineHosts = new();
    readonly Dictionary<string, V.LauncherPage> enginePages = new();
    readonly List<object> engineInventory = new();
    readonly Dictionary<string, V.LauncherPage> engineOptionSources = new();

    void InitializeEngineOptions()
    {
        if (!File.Exists(Path.Combine(root, "engine.lock.json"))) return;
        loadingEngineOptions = true;
        try
        {
            Environment.SetEnvironmentVariable("TECHMO_EXPERIMENT_ROOT", root);
            N.Workspace.EmbeddedChange = () => { if (!loadingEngineOptions && !syncing) MarkSettingsChanged(); };
            N.Workspace.Current.Initialise();
            N.Workspace.Current.Saved += () =>
            {
                if (loadingEngineOptions || savingEngineOptions || !engineOptionsReady) return;
                LoadState();
                state.Fps = NormalizeFps(N.Workspace.Current.Settings.Str("fps_mode"));
                ApplyStateToUi(); RememberAppliedSettings();
            };
            V.SettingRow.Classic = true; // Source choices become normal dropdowns; their window is never created.
            engineOptionsReady = true;
            BuildEngineOptionCards();
            GraphicsHardwareSummary.Text = N.Hardware.Detect().Summary;
            GraphicsRecommendationCard.Visibility = Visibility.Visible;
        }
        finally { loadingEngineOptions = false; }
    }

    static HashSet<string> Names(params string[] names) => new(names, StringComparer.Ordinal);

    void BuildEngineOptionCards()
    {
        engineInventory.Clear();
        var display = new V.DisplayPage();
        HostAdditional(DisplayPage, OwnOptions(display, row => !Names("Output resolution", "Fullscreen", "Widescreen",
            "Live resolution change", "Presentation mode", "Frame rate", "HDR output", "HDR source", "Paper white",
            "Peak brightness", "Highlight boost", "Game language").Contains(row.Title)));
        var graphics = new V.GraphicsPage();
        var knownGraphics = Names("Best settings for this PC", "Quality preset", "Sharpening (RCAS)", "Sharpening strength",
            "Show FPS", "CPU usage", "GPU usage", "Vignette", "Ambient occlusion strength", "Shadow distance", "Bloom",
            "Saturation", "Field of view", "Distance", "Height", "Model detail", "Chromatic aberration", "Depth of field",
            "Motion blur", "Ambient occlusion (SSAO)", "Game's own anti-aliasing", "Shadows from dynamic lights",
            "Screen-space reflections", "Skip intro videos");
        HostAdditional(GraphicsPage, OwnOptions(graphics, row => !knownGraphics.Contains(row.Title) &&
            !(row.Title == "Upscaler" && row.Editor is ComboBox)));

        // The Techmo binding table, search, separate key/mouse slots and capture overlay stay intact.
        var controls = new V.ControlsPage();
        var knownControls = Names("PC controls", "Mouse camera", "Mouse in menus", "Mouse sensitivity", "Invert horizontal",
            "Invert vertical", "Camera auto-rotation", "Profile", "Save and share", "Controller");
        HostPanel(KeyboardPanel, OwnOptions(controls, row => !knownControls.Contains(row.Title) && row.Section != "Gamepad"));
        HostPanel(GamepadPanel, OwnOptions(controls, row => row.Section == "Gamepad" && row.Title != "Controller"));

        var play = new V.PlayPage();
        HostAdditional(HomePage, OwnOptions(play, row => Names("The Old Hunters (DLC)", "DLC folder", "Add to Steam",
            "Player profile", "Saves folder", "Save backups", "Import saves", "In-game menu language").Contains(row.Title)));
        var settings = new V.SettingsPage();
        var ownSettings = Names("Frame statistics in the log", "FSR 4 profile in the log", "Audio statistics in the log",
            "Engine switches", "Engine folder", "Launcher settings", "Port settings (bbport.ini)");
        HostAdditional(GeneralPage, OwnOptions(settings, row => row.Section is not ("Appearance" or "Updates" or "Build from source" or "Community" or "Techmo Engine" or "Launch")
            && !ownSettings.Contains(row.Title)));

        var online = new V.OnlinePage();
        if (OnlinePage.Content is UIElement originalOnline) originalOnline.Visibility = Visibility.Collapsed;
        var ownOnline = OwnOptions(online, _ => true, includeDetails: true);
        var onlineBody = new StackPanel();
        if (OnlinePage.Content is UIElement oldContent) { OnlinePage.Content = null; onlineBody.Children.Add(oldContent); }
        onlineBody.Children.Add(ownOnline); OnlinePage.Content = onlineBody;
        enginePages["Online"] = online;

        var mods = new V.ModsPage();
        HostAdditional(ModsPage, OwnOptions(mods, row => row.Section == "Third-party patches", includeDetails: true,
            detailSection: "Third-party patches"));
        enginePages["Mods"] = mods;
        AddEngineNavigation("Trophies", "Trophies", "\uE735", new V.TrophiesPage());
        AddEngineNavigation("Engine", "Engine switches", "\uE713", new V.EnginePage());
    }

    void HostAdditional(ScrollViewer page, UIElement content)
    {
        if (page.Content is Panel panel) { HostPanel(panel, content); return; }
        var wrapper = new StackPanel();
        if (page.Content is UIElement existing) { page.Content = null; wrapper.Children.Add(existing); }
        page.Content = wrapper; HostPanel(wrapper, content);
    }

    void HostPanel(Panel parent, UIElement content)
    {
        parent.Children.Add(content); engineHosts.Add((parent, content));
    }

    void AddEngineNavigation(string id, string title, string glyph, V.LauncherPage source)
    {
        var label = new TextBlock { Text = title, VerticalAlignment = VerticalAlignment.Center };
        string titleKey = id == "Trophies" ? "Ui.Trophies" : "Ui.Engine_switches";
        label.SetBinding(TextBlock.TextProperty, new Binding($"[{titleKey}]") { Source = Localization.Current });
        label.SetResourceReference(TextBlock.ForegroundProperty, "TextBrush");
        var icon = new TextBlock { Text = glyph, FontFamily = new FontFamily("Segoe MDL2 Assets"), FontSize = 15,
            Width = 28, VerticalAlignment = VerticalAlignment.Center };
        var content = new StackPanel { Orientation = Orientation.Horizontal };
        content.Children.Add(icon); content.Children.Add(label);
        var nav = new Button { Content = content, CommandParameter = id, Style = (Style)FindResource("NavButton") };
        nav.Click += Nav_Click;
        var sidebar = (Panel)SidebarNavigation.Content;
        sidebar.Children.Insert(id == "Trophies" ? 5 : sidebar.Children.Count, nav);
        var body = new StackPanel { Margin = new Thickness(26, 6, 26, 26) };
        var heading = new TextBlock { Style = (Style)FindResource("PageTitle") };
        heading.SetBinding(TextBlock.TextProperty, new Binding("Text") { Source = label });
        body.Children.Add(heading);
        var description = new TextBlock { Style = (Style)FindResource("PageDescription") };
        string descriptionKey = id == "Trophies" ? "Trophies.Description" : "Engine.Description";
        description.SetBinding(TextBlock.TextProperty, new Binding($"[{descriptionKey}]") { Source = Localization.Current });
        body.Children.Add(description);
        body.Children.Add(OwnOptions(source, _ => true, includeDetails: true));
        var view = new ScrollViewer { Content = body,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Visibility = Visibility.Collapsed, Margin = DisplayPage.Margin };
        Grid.SetRow(view, Grid.GetRow(HomePage));
        ((Panel)HomePage.Parent).Children.Add(view);
        engineNavigation.Add((id, nav, view)); enginePages[id] = source;
    }

    StackPanel OwnOptions(V.LauncherPage source, Func<V.SettingRow, bool> include, bool includeDetails = false,
        string? detailSection = null)
    {
        engineOptionSources[source.GetType().Name] = source;
        var output = new StackPanel { Margin = new Thickness(0, 20, 0, 0) };
        StackPanel? group = null;
        string? section = null;
        void Group(string? name)
        {
            section = name;
            group = new StackPanel();
            var header = new TextBlock { Text = name ?? "Options", Style = (Style)FindResource("SectionTitle") };
            group.Children.Add(header);
            output.Children.Add(new Border { Style = (Style)FindResource("CardStyle"), Child = group,
                Margin = new Thickness(0, 0, 0, 20) });
        }
        void Put(V.SettingRow row)
        {
            if (!include(row)) return;
            if (group == null || section != row.Section) Group(row.Section);
            group!.Children.Add(OwnSetting(row));
            engineInventory.Add(new { page = source.GetType().Name, row.Title, row.Section, row.Choices });
        }
        if (!includeDetails)
        {
            foreach (var row in source.HostedRows) Put(row);
            return output;
        }
        string? currentSection = null;
        foreach (var child in source.HostedBody.Children.Cast<UIElement>().ToArray())
        {
            if (child is TextBlock { Tag: "group" } heading) { currentSection = heading.Text; continue; }
            if (child is V.SettingRow row) { Put(row); continue; }
            if (child is V.AdvancedSection advanced)
            {
                foreach (var nested in source.HostedRows.Where(r => IsUnder(r, advanced))) Put(nested);
                continue;
            }
            if (detailSection != null && currentSection != detailSection) continue;
            if (group == null || section != currentSection) Group(currentSection);
            Detach(child); RestyleEditor(child);
            group!.Children.Add(child);
        }
        return output;
    }

    static bool IsUnder(DependencyObject item, DependencyObject parent)
    {
        for (var at = LogicalTreeHelper.GetParent(item); at != null; at = LogicalTreeHelper.GetParent(at))
            if (ReferenceEquals(at, parent)) return true;
        return false;
    }

    Grid OwnSetting(V.SettingRow source)
    {
        var row = new Grid { Margin = new Thickness(0, 9, 0, 9), MinHeight = 38, Tag = source };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 20, 0) };
        text.Children.Add(new TextBlock { Text = source.Title, TextWrapping = TextWrapping.Wrap });
        if (!string.IsNullOrWhiteSpace(source.Info))
        {
            var help = new TextBlock { Text = source.Info, FontSize = 12, TextWrapping = TextWrapping.Wrap,
                MaxWidth = 480, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 4, 0, 0) };
            help.SetResourceReference(TextBlock.ForegroundProperty, "MutedBrush"); text.Children.Add(help);
        }
        var hint = new TextBlock { FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 0) };
        hint.SetResourceReference(TextBlock.ForegroundProperty, "MutedBrush");
        hint.SetBinding(TextBlock.TextProperty, new Binding("Text") { Source = source.Hint });
        hint.SetBinding(VisibilityProperty, new Binding("Visibility") { Source = source.Hint });
        text.Children.Add(hint); row.Children.Add(text);
        if (source.Editor is { } raw)
        {
            Detach(raw);
            var editor = OwnEditor(raw);
            if (editor is FrameworkElement element) element.Margin = new Thickness(0);
            Grid.SetColumn(editor, 1); row.Children.Add(editor);
        }
        row.SetBinding(IsEnabledProperty, new Binding("IsEnabled") { Source = source });
        // Keep conditional fields conditional, while unfolding upstream Advanced sections into our cards.
        var visible = new MultiBinding { Converter = new HostedVisibility() };
        for (DependencyObject? at = source; at != null && at is not V.LauncherPage; at = LogicalTreeHelper.GetParent(at))
            if (at is UIElement && at is not V.AdvancedRows)
                visible.Bindings.Add(new Binding("Visibility") { Source = at });
        row.SetBinding(VisibilityProperty, visible);
        return row;
    }

    UIElement OwnEditor(UIElement source)
    {
        if (source is Wpf.Ui.Controls.ToggleSwitch toggle)
        {
            var check = new CheckBox { VerticalAlignment = VerticalAlignment.Center };
            check.SetBinding(ToggleButton.IsCheckedProperty, new Binding("IsChecked") { Source = toggle, Mode = BindingMode.TwoWay });
            check.SetBinding(IsEnabledProperty, new Binding("IsEnabled") { Source = toggle });
            check.Click += (_, _) => toggle.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            return check;
        }
        if (source is Wpf.Ui.Controls.Button action)
        {
            var button = new Button { Content = action.Content ?? action.ToolTip ?? "Open", ToolTip = action.ToolTip,
                Padding = new Thickness(14, 8, 14, 8), Margin = new Thickness(4, 0, 0, 0) };
            button.SetBinding(IsEnabledProperty, new Binding("IsEnabled") { Source = action });
            button.SetBinding(VisibilityProperty, new Binding("Visibility") { Source = action });
            button.Click += (_, _) => action.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            return button;
        }
        if (source is Panel panel)
        {
            foreach (var item in panel.Children.Cast<UIElement>().ToArray())
            {
                var index = panel.Children.IndexOf(item); panel.Children.RemoveAt(index);
                panel.Children.Insert(index, OwnEditor(item));
            }
        }
        RestyleEditor(source);
        return source;
    }

    void RestyleEditor(UIElement source)
    {
        if (source is TextBlock text) text.SetResourceReference(TextBlock.ForegroundProperty, "TextBrush");
        if (source is Control control)
        {
            control.SetResourceReference(Control.ForegroundProperty, "TextBrush");
            control.FontFamily = FontFamily; control.FontSize = 13;
            var type = control switch { ComboBox => typeof(ComboBox), TextBox => typeof(TextBox), Slider => typeof(Slider),
                CheckBox => typeof(CheckBox), Button => typeof(Button), _ => null };
            if (type != null && TryFindResource(type) is Style style) control.Style = style;
        }
    }

    static void Detach(UIElement element)
    {
        switch (LogicalTreeHelper.GetParent(element))
        {
            case Panel panel: panel.Children.Remove(element); break;
            case Border border: border.Child = null; break;
            case ContentControl content: content.Content = null; break;
        }
    }

    sealed class HostedVisibility : IMultiValueConverter
    {
        public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture) =>
            values.Any(value => value is Visibility visibility && visibility != Visibility.Visible) ? Visibility.Collapsed : Visibility.Visible;
        public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) => throw new NotSupportedException();
    }

    void SaveEngineOptions()
    {
        if (!engineOptionsReady) return;
        savingEngineOptions = true;
        try { N.Workspace.Current.SaveNow(); } // Flush extras before Techmo merges its own edited fields.
        finally { savingEngineOptions = false; }
    }

    void ReloadEngineOptions()
    {
        if (!engineOptionsReady) return;
        loadingEngineOptions = true;
        try
        {
            foreach (var host in engineHosts) host.parent.Children.Remove(host.content);
            engineHosts.Clear();
            foreach (var item in engineNavigation)
            {
                ((Panel)SidebarNavigation.Content).Children.Remove(item.button);
                ((Panel)HomePage.Parent).Children.Remove(item.view);
            }
            engineNavigation.Clear(); enginePages.Clear();
            if (OnlinePage.Content is Panel online && online.Children.Count > 0)
            {
                var original = online.Children[0]; online.Children.RemoveAt(0);
                OnlinePage.Content = original; original.Visibility = Visibility.Visible;
            }
            N.Workspace.Current.Settings.Load(); N.Workspace.Current.Ini.Load();
            BuildEngineOptionCards(); NavigateHub(currentHubPage);
        }
        finally { loadingEngineOptions = false; }
    }

    void SyncEngineSettings()
    {
        if (!engineOptionsReady) return;
        var settings = N.Workspace.Current.Settings;
        settings.Set("game_dir", state.GamePath); settings.Set("fps_mode", state.Fps);
        settings.Set("present_mode", string.IsNullOrEmpty(state.PresentMode) ? "Mailbox" : state.PresentMode);
        settings.Set("fullscreen", state.Fullscreen); settings.Set("mods_enabled", state.ModsEnabled);
        settings.Set("frame_stats", state.FrameStats); settings.Set("gpu_profile", state.GpuProfile);
        settings.Set("audio_stats", state.AudioStats); settings.Set("fsr_profile", state.FsrProfile);
        settings.Save(); N.Workspace.Current.Ini.Load();
    }

    void ConfigureHostedEngineLaunch(System.Diagnostics.ProcessStartInfo launch)
    {
        SyncEngineSettings();
        foreach (var (key, value) in N.GameEnvironment.Build(N.Workspace.Current.Settings)) launch.Environment[key] = value;
        launch.Environment["TECHMO_USE_SELECTED_USER"] = "1";
    }

    string EngineSavesDirectory => engineOptionsReady
        ? N.GameEnvironment.UserDir(N.Workspace.Current.Settings)
        : Environment.GetEnvironmentVariable("BB_USER_DIR") ?? Path.Combine(root, "user");
}
