using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using BloodborneLauncher;
using N = Techmo.Engine.Core;
using V = Techmo.Engine.Views;

static class Program
{
    [STAThread] static int Main(string[] args)
    {
        try { Run(Path.GetFullPath(args[0])); return 0; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    static MainWindow window = null!;
    static T Field<T>(string name) => (T)typeof(MainWindow).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(window)!;
    static object? Call(string name, params object[] arguments) => typeof(MainWindow).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(window, arguments);
    static T Get<T>(string name) where T : class => (T)window.FindName(name);
    static int checks;
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); checks++; }
    static void Pump() => window.Dispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
    static string SourceHash(string path)
    {
        var utf8 = System.Text.Encoding.UTF8;
        var bytes = utf8.GetBytes(utf8.GetString(File.ReadAllBytes(path)).Replace("\r\n", "\n"));
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes));
    }
    static IEnumerable<T> Visuals<T>(DependencyObject root) where T : DependencyObject
    {
        if (root is T match) yield return match;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            foreach (var child in Visuals<T>(VisualTreeHelper.GetChild(root, i))) yield return child;
    }
    static void Run(string source)
    {
        var output = Path.Combine(source, "builds", "_internal", "techmo-hosted-previews");
        var fixture = Path.Combine(source, "builds", "_internal", "hosted-fixture-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(output); Directory.CreateDirectory(Path.Combine(fixture, "out"));
        File.WriteAllText(Path.Combine(fixture, "engine.lock.json"), "{}");
        File.WriteAllText(Path.Combine(fixture, "run.bat"), "@echo off\nexit /b 0\n");
        File.WriteAllText(Path.Combine(fixture, "build.sh"), "# fixture\n");
        File.WriteAllBytes(Path.Combine(fixture, "out", "bb-probe.exe"), Array.Empty<byte>());
        File.WriteAllBytes(Path.Combine(fixture, "out", "bb-start.exe"), Array.Empty<byte>());
        File.WriteAllText(Path.Combine(fixture, "launcher-settings.json"), "{\"UiLanguage\":\"en\",\"UiTheme\":\"dark\",\"ReducedMotion\":true}");
        File.WriteAllText(Path.Combine(fixture, "bbport.ini"), "frame_gen=off\n");
        Environment.SetEnvironmentVariable("TECHMO_EXPERIMENT_ROOT", fixture);
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(Dispatcher.CurrentDispatcher));
        var app = new App(); app.InitializeComponent();
        window = new MainWindow { ShowInTaskbar = false, ShowActivated = false, Left = -20000, Top = -20000,
            WindowStartupLocation = WindowStartupLocation.Manual };
        app.MainWindow = window; window.Show(); Pump();
        Check(window.GetType().BaseType == typeof(Window), "The shell must be the Techmo window");
        Check(Field<string>("root") == fixture && N.Paths.ConfigFile.StartsWith(fixture), "Every fixture path stays isolated");
        Check(Get<Image>("HeroImage").Source is BitmapSource, "Techmo hero artwork is present");
        Check(Get<DataGrid>("KeyBindingsGrid").Items.Count == 29, "Techmo keeps all 29 action bindings");
        Check(Get<Button>("KeyboardTab") != null && Get<Button>("UndoSettingsButton") != null, "Techmo tabs and Apply/Undo stay present");
        Check(Field<bool>("engineOptionsReady"), "Additional native engine options initialized");
        var pages = Field<Dictionary<string, V.LauncherPage>>("engineOptionSources");
        Check(pages.Count >= 9, "Native option catalogs remain available through the Techmo presentation");
        void Shot(string page)
        {
            Call("NavigateHub", page); Pump(); window.UpdateLayout();
            var bitmap = new RenderTargetBitmap((int)window.ActualWidth, (int)window.ActualHeight, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(window); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var stream = File.Create(Path.Combine(output, page + ".png")); encoder.Save(stream);
        }
        foreach (var page in new[] { "Home", "Display", "Graphics", "Controller", "General", "Online", "Trophies", "Engine" }) Shot(page);
        foreach (var entry in Field<List<(string id, Button button, FrameworkElement view)>>("engineNavigation"))
        {
            entry.button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)); Pump(); window.UpdateLayout();
            var scroll = (ScrollViewer)entry.view;
            var top = scroll.TransformToAncestor(window).Transform(new Point());
            var header = Get<Grid>("WorkspaceHeader");
            var headerBottom = header.TransformToAncestor(window).Transform(new Point(0, header.ActualHeight)).Y;
            Check(Grid.GetRow(scroll) == Grid.GetRow(Get<ScrollViewer>("HomePage")) && top.Y >= headerBottom &&
                scroll.IsVisible && scroll.ViewportHeight > 300, entry.id + " renders below the header with a full content viewport");
            Check(scroll.ScrollableHeight > 0, entry.id + " content is present and scrollable");
            if (entry.id == "Engine")
                Check(Visuals<Grid>(scroll).Count(grid => grid.Tag is V.SettingRow) == pages["EnginePage"].HostedRows.Count,
                    "Every engine switch and its summary is rendered in the Techmo page");
            else
            {
                Check(Visuals<V.SettingRow>(scroll).Count() == N.TrophyTracker.All.Length,
                    "Trophies displays the full trophy list even without any unlocks");
                var filter = Visuals<ComboBox>(scroll).Single(box => box.Items.Cast<ComboBoxItem>().Any(item => item.Tag?.ToString() == "earned"));
                filter.SelectedIndex = 1; Pump();
                Check(!Visuals<V.SettingRow>(scroll).Any() && Visuals<TextBlock>(scroll).Any(text => text.Text == "No trophies earned yet."),
                    "The earned filter shows an explicit empty state");
                filter.SelectedIndex = 0; Pump();
                Check(Visuals<V.SettingRow>(scroll).Count() == N.TrophyTracker.All.Length, "The All filter restores every trophy");
            }
        }
        var texture = (ComboBox)pages["EnginePage"].HostedRows.Single(row => row.Title == "Texture filtering").Editor!;
        texture.SelectedIndex = 1;
        var textureValue = ((ComboBoxItem)texture.SelectedItem).Tag!.ToString();
        var generated = pages["GraphicsPage"].HostedRows.Single(row => row.Title == "Frame generation");
        var choices = (ComboBox)generated.Editor!;
        var expected = ((ComboBoxItem)choices.Items[1]).Tag!.ToString();
        choices.SelectedIndex = 1;
        Check(Field<bool>("hasUnsavedChanges"), "A supplemental edit participates in Techmo's Apply/Undo state");
        Check(File.ReadAllText(Path.Combine(fixture, "bbport.ini")).Contains("frame_gen=off"), "Supplemental edits stay pending until Apply");
        Check((bool)Call("SaveSettings")!, "Apply saves Techmo and native options together");
        Check(N.GameEnvironment.Build(N.Workspace.Current.Settings)["BB_ANISO"] == textureValue &&
            JsonDocument.Parse(File.ReadAllText(N.Paths.ConfigFile)).RootElement.GetProperty("env:BB_ANISO").GetString() == textureValue,
            "The engine switch edit persists and reaches the game's environment");
        Check(File.ReadAllLines(Path.Combine(fixture, "bbport.ini")).Contains("frame_gen=" + expected), "Apply preserves the native setting value");
        var transform = Get<DataGrid>("KeyBindingsGrid").Items.Cast<MainWindow.KeyBindingRow>().Single(row => row.Key == "transform");
        transform.Binding = "C, Left Shift + Mouse2";
        Check((bool)Call("SaveSettings")!, "Apply accepts a Techmo right-mouse chord");
        Check(File.ReadAllLines(Path.Combine(fixture, "bbport.ini")).Contains("bind.transform=C, Left Shift + Mouse3"),
            "Right mouse is translated to SDL Mouse3 for the engine");
        Call("LoadState"); Call("ApplyStateToUi");
        transform = Get<DataGrid>("KeyBindingsGrid").Items.Cast<MainWindow.KeyBindingRow>().Single(row => row.Key == "transform");
        Check(transform.Binding == "C, Left Shift + Mouse2", "Reload restores Techmo's mouse numbering without changing the chord");
        var launch = new ProcessStartInfo(); Call("ConfigureEngineLaunch", launch);
        Check(launch.Environment["BB_USER_DIR"]?.StartsWith(fixture) == true, "Launch saves remain isolated");
        Check(launch.Environment["TECHMO_USE_SELECTED_USER"] == "1", "Selected save/profile folder reaches the session wrapper");
        var fps = Get<ComboBox>("FpsCombo");
        Check(fps.Items.Cast<ComboBoxItem>().Any(item => item.Tag?.ToString() == "120") &&
            fps.Items.Cast<ComboBoxItem>().Any(item => item.Tag?.ToString() == "144"), "Upstream 120/144 FPS choices remain available");
        foreach (var mode in new[] { "120", "144" })
        {
            fps.SelectedItem = fps.Items.Cast<ComboBoxItem>().Single(item => item.Tag?.ToString() == mode);
            Check((bool)Call("SaveSettings")!, "Techmo profile validation accepts " + mode + " FPS");
            Call("LoadState"); Call("ApplyStateToUi");
            Check(((ComboBoxItem)fps.SelectedItem).Tag?.ToString() == mode, mode + " FPS survives save/reload");
        }
        Check(Get<Border>("GraphicsRecommendationCard").Visibility == Visibility.Visible &&
            !string.IsNullOrWhiteSpace(Get<TextBlock>("GraphicsHardwareSummary").Text), "Techmo Graphics displays hardware and the recommendation action");
        void Select(string name, string value)
        {
            var combo = Get<ComboBox>(name);
            combo.SelectedItem = combo.Items.Cast<ComboBoxItem>().Single(item => (item.Tag ?? item.Content)?.ToString() == value);
        }
        string Selected(string name) { var item = (ComboBoxItem)Get<ComboBox>(name).SelectedItem; return (item.Tag ?? item.Content).ToString()!; }
        Select("UpscalerCombo", "fsr4"); Select("PresetCombo", "4"); Select("FpsCombo", "30"); Select("ResolutionCombo", "3840x2160");
        var motion = (Wpf.Ui.Controls.ToggleSwitch)Field<Dictionary<string, V.LauncherPage>>("engineOptionSources")["GraphicsPage"]
            .HostedRows.Single(row => row.Title == "Object motion vectors").Editor!;
        motion.IsChecked = true;
        Check((bool)Call("SaveSettings")!, "Recommendation fixture baseline saved");
        Select("PresetCombo", "0"); // A pending value, different from the file's preset=4.
        Get<CheckBox>("SsaoCheck").IsChecked = false;
        var low = new N.Hardware("Intel UHD", "Intel", 1L << 30, "Test CPU", 8, 16L << 30, 1600, 900, 60);
        object Preview(N.Hardware hardware) => Call("CreateGraphicsRecommendation", hardware)!;
        IReadOnlyList<N.Hardware.Change> Changes(object preview) => (IReadOnlyList<N.Hardware.Change>)preview.GetType().GetProperty("Changes")!.GetValue(preview)!;
        var preview = Preview(low);
        Check(Changes(preview).Count == 5 && Changes(preview).Single(change => change.Label == "Quality preset").From ==
            N.Choices.Presets.Single(choice => choice.Value == "0").Label.Split(" (")[0], "Recommendation compares against pending controls, not saved values");
        var iniPath = Path.Combine(fixture, "bbport.ini"); var statePath = Path.Combine(fixture, "launcher-settings.json");
        var nativePath = N.Paths.ConfigFile;
        var beforeIni = File.ReadAllText(iniPath); var beforeState = File.ReadAllText(statePath); var beforeNative = File.ReadAllText(nativePath);
        var beforeMotion = N.Workspace.Current.Ini.Get("object_motion");
        var dialog = (Window)Call("BuildGraphicsRecommendationDialog", preview)!;
        window.Dispatcher.BeginInvoke(new Action(() => dialog.DialogResult = false), DispatcherPriority.ApplicationIdle);
        Check(dialog.ShowDialog() == false, "Recommendation preview can be cancelled");
        Check(Selected("PresetCombo") == "0" && Selected("FpsCombo") == "30" && N.Workspace.Current.Ini.Get("object_motion") == beforeMotion &&
            File.ReadAllText(iniPath) == beforeIni && File.ReadAllText(statePath) == beforeState && File.ReadAllText(nativePath) == beforeNative,
            "Preview and Cancel leave pending choices, live native state and all files untouched");
        // Exercise the real card button and modal confirmation, including its own Techmo presentation.
        window.Dispatcher.BeginInvoke(new Action(() =>
        {
            var opened = window.OwnedWindows.Cast<Window>().Single(item => item.IsVisible);
            opened.UpdateLayout();
            var bitmap = new RenderTargetBitmap((int)opened.ActualWidth, (int)opened.ActualHeight, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(opened); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using (var stream = File.Create(Path.Combine(output, "Recommendation.png"))) encoder.Save(stream);
            var body = (StackPanel)((ScrollViewer)opened.Content).Content;
            var buttons = ((StackPanel)body.Children[^1]).Children.OfType<Button>().ToArray();
            Check(opened.GetType() == typeof(Window) && buttons.Length == 2, "Recommendation uses a Techmo preview with Cancel and Use recommendation");
            buttons.Single(button => button.IsDefault).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }), DispatcherPriority.ApplicationIdle);
        Get<Button>("RecommendGraphicsButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(Field<bool>("hasUnsavedChanges") && Selected("FpsCombo") != "30" &&
            File.ReadAllText(iniPath) == beforeIni && File.ReadAllText(statePath) == beforeState && File.ReadAllText(nativePath) == beforeNative,
            "Confirming the real recommendation stages changes without saving");
        Check(Get<CheckBox>("SsaoCheck").IsChecked == false, "Recommendation preserves unrelated pending edits");
        Call("StageGraphicsRecommendation", preview);
        Check(Selected("UpscalerCombo") == "fsr3" && Selected("PresetCombo") == "2" && Selected("FpsCombo") == "60" &&
            Selected("ResolutionCombo") == "1600x900" && !N.Workspace.Current.Ini.Flag("object_motion") && motion.IsChecked == false,
            $"Low-end hardware stages all five native recommendations in the visible Techmo controls: {Selected("UpscalerCombo")}, {Selected("PresetCombo")}, {Selected("FpsCombo")}, {Selected("ResolutionCombo")}, native motion={N.Workspace.Current.Ini.Get("object_motion")}, control motion={motion.IsChecked}");
        Call("UndoSettings_Click", window, new RoutedEventArgs());
        Check(Selected("UpscalerCombo") == "fsr4" && Selected("PresetCombo") == "4" && Selected("FpsCombo") == "30" &&
            Selected("ResolutionCombo") == "3840x2160" && N.Workspace.Current.Ini.Flag("object_motion") && !Field<bool>("hasUnsavedChanges"),
            "Undo restores the complete applied state, including supplemental motion vectors");
        Call("StageGraphicsRecommendation", Preview(low));
        Check((bool)Call("SaveSettings")!, "Recommended 1600x900 is valid in Techmo profiles and Apply");
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Selected("ResolutionCombo") == "1600x900" && Selected("FpsCombo") == "60" &&
            File.ReadAllLines(iniPath).Contains("object_motion=0") && Changes(Preview(low)).Count == 0,
            "Apply and reload persist the complete recommendation; repeated recommendations are empty");
        var matched = (Window)Call("BuildGraphicsRecommendationDialog", Preview(low))!;
        var matchedBody = (StackPanel)((ScrollViewer)matched.Content).Content;
        Check(((StackPanel)matchedBody.Children[^1]).Children.OfType<Button>().All(button => button.IsCancel), "An already-matching PC has only Close, without a misleading Apply action");
        matched.Close();
        foreach (var hardware in new[]
        {
            new N.Hardware("AMD Radeon RX 9070", "AMD", 16L << 30, "Test CPU", 16, 32L << 30, 2560, 1600, 165),
            new N.Hardware("AMD Radeon RX 9070", "AMD", 16L << 30, "Test CPU", 16, 32L << 30, 2880, 1800, 165)
        })
        {
            Call("StageGraphicsRecommendation", Preview(hardware));
            string expectedResolution = hardware.ScreenWidth == 2560 ? "2560x1600" : "auto";
            Check(Selected("ResolutionCombo") == expectedResolution && Selected("FpsCombo") == "120" && Selected("UpscalerCombo") == "fsr4" &&
                N.Workspace.Current.Ini.Flag("object_motion"), "High-end hardware supports RDNA4, 120 FPS and " + expectedResolution);
            Check((bool)Call("SaveSettings")!, "Recommended native/automatic resolution passes profile validation");
            Call("LoadState"); Call("ApplyStateToUi");
            Check(Selected("ResolutionCombo") == expectedResolution && Selected("FpsCombo") == "120", "Recommended resolution and FPS survive reload");
        }
        BloodborneLauncher.Localization.Current.SetLanguage("vi");
        Check(Get<Button>("RecommendGraphicsButton").Content.ToString() == "Đề xuất…", "Recommendation card follows Techmo's Vietnamese language setting");
        BloodborneLauncher.Localization.Current.SetLanguage("en");
        Shot("Graphics");
        File.WriteAllText(Path.Combine(output, "options.json"), JsonSerializer.Serialize(Field<List<object>>("engineInventory"), new JsonSerializerOptions { WriteIndented = true }));
        // Preserve the exact Techmo input/capture implementation, not the upstream binding editor.
        var inputHashes = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(Path.Combine(source, "tests", "fixtures", "techmo-input-sha256.json")))!;
        foreach (var (file, expectedHash) in inputHashes)
            Check(SourceHash(Path.Combine(source, "apps", "launcher", file))
                .Equals(expectedHash, StringComparison.OrdinalIgnoreCase), "Techmo input source preserved: " + file);
        typeof(MainWindow).GetField("hasUnsavedChanges", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(window, false);
        window.Close(); app.Shutdown();
        Console.WriteLine($"PASS: {checks} checks; Techmo shell, input, native options, recommendations, Apply/Undo, launch environment and page previews.");
    }
}
