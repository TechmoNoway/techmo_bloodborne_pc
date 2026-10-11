param([string]$PreviewDirectory, [string]$PackageCache, [string]$SdlDll)
$ErrorActionPreference = 'Stop'
# Keep the verification host outside the repository: Play can only reach
# synthetic fixtures, never the user's runtime, game dump or saved settings.
$verificationRoot = Join-Path ([IO.Path]::GetTempPath()) ('BloodborneLauncher-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $verificationRoot | Out-Null
if (!$PreviewDirectory) { $PreviewDirectory = Join-Path $verificationRoot 'previews' }
$PreviewDirectory = [IO.Path]::GetFullPath($PreviewDirectory)
$projectReference = [Security.SecurityElement]::Escape((Join-Path $PSScriptRoot 'Techmo.Launcher.csproj'))
$project = '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0-windows</TargetFramework><UseWPF>true</UseWPF><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable></PropertyGroup><ItemGroup><ProjectReference Include="'+$projectReference+'"/></ItemGroup></Project>'
Set-Content -Encoding utf8 -LiteralPath (Join-Path $verificationRoot 'LauncherChecks.csproj') -Value $project
$program = @'
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Input;
using System.Runtime.InteropServices;
using BloodborneLauncher;
using Localization = BloodborneLauncher.Localization;

static class Program
{
    static MainWindow window = null!;
    static int assertions;
    static T Get<T>(string name) where T : class => (T)window.FindName(name);
    static void Call(string name, params object[] args) => typeof(MainWindow).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(window, args);
    static void Check(bool value, string message) { if (!value) throw new Exception(message); assertions++; }
    static string Tag(string name) => ((ComboBoxItem)Get<ComboBox>(name).SelectedItem).Tag?.ToString() ?? "";
    static void Select(string name, string tag) {
        var box = Get<ComboBox>(name);
        box.SelectedItem = box.Items.Cast<ComboBoxItem>().First(x => x.Tag?.ToString() == tag);
    }
    static void ClickNav(string page) => Call("Nav_Click", Get<Button>("Nav" + page), new RoutedEventArgs());
    static void Pump() => window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
    static IEnumerable<DependencyObject> Visuals(DependencyObject parent) {
        for(int i=0;i<VisualTreeHelper.GetChildrenCount(parent);i++) {
            var child=VisualTreeHelper.GetChild(parent,i); yield return child;
            foreach(var nested in Visuals(child)) yield return nested;
        }
    }
    static MainWindow.KeyBindingRow BindingRow(string key) => Get<DataGrid>("KeyBindingsGrid").Items.Cast<MainWindow.KeyBindingRow>().Single(row=>row.Key==key);
    static void ClickBinding(MainWindow.KeyBindingRow row,string slot,bool clear=false) {
        var grid=Get<DataGrid>("KeyBindingsGrid"); grid.ScrollIntoView(row); window.UpdateLayout(); Pump();
        var button=Visuals(grid).OfType<Button>().First(b=>ReferenceEquals(b.DataContext,row) && b.CommandParameter?.ToString()==slot && (b.Content?.ToString()=="×")==clear);
        button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)); Pump();
        // Synthetic events must not inherit modifiers from someone typing in
        // another application while this off-screen verifier runs.
        if(!clear) {
            ((HashSet<Key>)typeof(MainWindow).GetField("captureModifiers",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!).Clear();
            Call("UpdateCapturePrompt");
        }
    }
    static void ReleaseCaptureKey(Key key) {
        var e=new KeyEventArgs(Keyboard.PrimaryDevice,PresentationSource.FromVisual(window),Environment.TickCount,key) {RoutedEvent=Keyboard.PreviewKeyUpEvent};
        window.RaiseEvent(e); Pump();
    }
    static void SendCaptureKey(Key key,bool hold=false) {
        var e=new KeyEventArgs(Keyboard.PrimaryDevice,PresentationSource.FromVisual(window),Environment.TickCount,key) {RoutedEvent=Keyboard.PreviewKeyDownEvent};
        window.RaiseEvent(e); Pump();
        Check(e.Handled,"Capture consumes keyboard input "+key);
        if(!hold)ReleaseCaptureKey(key);
    }
    static void SendCaptureMouse(MouseButton button) {
        var e=new MouseButtonEventArgs(Mouse.PrimaryDevice,Environment.TickCount,button) {RoutedEvent=Mouse.PreviewMouseDownEvent};
        window.RaiseEvent(e); Pump();
        Check(e.Handled,"Capture consumes mouse input "+button);
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SdlScancode([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    static void VerifyChords(string output,string root) {
        Select("BindingPresetCombo","default");
        var strong=BindingRow("strong_attack");
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftShift,hold:true);
        Check(Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible && strong.MouseToken=="" && Get<TextBlock>("CapturePrompt").Text.Contains("Left Shift"),"Holding a modifier waits for the mouse instead of finishing capture");
        Render(Path.Combine(output,"controls-chord-capture-en.png"));
        SendCaptureMouse(MouseButton.Left);
        Check(strong.Binding=="V, Left Shift + Mouse1" && strong.MouseDisplay=="Left Shift + Left mouse","Shift plus left mouse is a chord in the mouse slot");
        Check(BindingRow("attack").Binding=="Mouse1","Assigning a chord preserves the plain binding for unmodified input");
        ClickBinding(strong,"keyboard");SendCaptureKey(Key.LeftCtrl,hold:true);SendCaptureKey(Key.RightShift,hold:true);SendCaptureKey(Key.V);
        Check(strong.Binding=="Right Shift + Left Ctrl + V, Left Shift + Mouse1","Multiple side-specific modifiers preserve the opposite slot");
        Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");Pump();strong=BindingRow("strong_attack");
        Check(strong.Binding=="Right Shift + Left Ctrl + V, Left Shift + Mouse1","Keyboard and mouse chords survive saving and restarting");
        Check(File.ReadAllLines(Path.Combine(root,"bbport.ini")).Contains("bind.strong_attack=Right Shift + Left Ctrl + V, Left Shift + Mouse1"),"The engine receives plus-separated chords and comma-separated alternatives");
        Select("BindingPresetCombo","ds3");Select("BindingPresetCombo","custom");
        Check(strong.Binding=="Right Shift + Left Ctrl + V, Left Shift + Mouse1","Custom preset retains chords");
        Select("UiLanguageCombo","vi");Pump();
        Check(strong.MouseDisplay=="Shift trái + Chuột trái" && strong.KeyboardDisplay=="Shift phải + Ctrl trái + V","Vietnamese translates each chord component without changing SDL tokens");
        Select("BindingGroupCombo","combat");Render(Path.Combine(output,"controls-chords-vi.png"));
        Select("UiLanguageCombo","en");
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftShift,hold:true);SendCaptureKey(Key.Escape);
        Check(strong.MouseToken=="Left Shift + Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Esc cancels a pending chord");
        ClickBinding(strong,"mouse");SendCaptureMouse(MouseButton.Right);
        Check(strong.MouseToken=="Mouse2","Cancelled modifiers do not leak into the next capture");
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftCtrl,hold:true);ReleaseCaptureKey(Key.LeftCtrl);SendCaptureMouse(MouseButton.Left);
        Check(strong.MouseToken=="Mouse1","Released modifiers are omitted before the mouse press");
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftAlt,hold:true);Call("InputCapture_Deactivated",window,EventArgs.Empty);
        Check(strong.MouseToken=="Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Focus loss cancels a pending chord");
        ClickBinding(strong,"mouse");SendCaptureMouse(MouseButton.Right);Check(strong.MouseToken=="Mouse2","Focus loss clears modifier state");
        foreach(var modifier in new[]{Key.LeftShift,Key.RightShift,Key.LeftCtrl,Key.RightCtrl,Key.LeftAlt,Key.RightAlt}) {
            ClickBinding(strong,"keyboard");SendCaptureKey(modifier);
            Check(!strong.KeyboardToken.Contains(" + ") && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"A modifier alone is captured on release "+modifier);
        }
        ClickBinding(strong,"mouse");SendCaptureKey(Key.RightAlt,hold:true);SendCaptureMouse(MouseButton.XButton2);
        Check(strong.MouseToken=="Right Alt + Mouse5","Modifier plus side mouse button");
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftCtrl,hold:true);
        var wheel=new MouseWheelEventArgs(Mouse.PrimaryDevice,Environment.TickCount,-120) {RoutedEvent=Mouse.PreviewMouseWheelEvent};
        window.RaiseEvent(wheel);Pump();
        Check(wheel.Handled && strong.MouseToken=="Left Ctrl + WheelDown","Modifier plus wheel direction");
        ClickBinding(strong,"keyboard");SendCaptureKey(Key.LeftCtrl,hold:true);SendCaptureKey(Key.Add);
        Check(strong.KeyboardToken=="Left Ctrl + Keypad +","The keypad plus key is distinct from a chord separator");
        ClickBinding(strong,"keyboard");SendCaptureKey(Key.LeftShift,hold:true);Get<Button>("CaptureEscapeButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(strong.KeyboardToken=="Left Shift + Escape","Explicit Esc supports modifiers without changing Esc cancellation");
        Select("BindingGroupCombo","all");
        ClickBinding(strong,"mouse",clear:true);Check(strong.Binding=="Left Shift + Escape","Clearing a chord preserves the keyboard slot");
        ClickBinding(strong,"keyboard",clear:true);Check(strong.Binding=="none","Clearing both chords serializes none");
        var names=typeof(MainWindow).Assembly.GetType("BloodborneLauncher.InputKeyNames")!;
        var normalize=names.GetMethod("NormalizeChord",BindingFlags.Static|BindingFlags.NonPublic)!;
        foreach(var invalid in new[]{"W + Mouse1","Left Ctrl + Left Ctrl + V","Left Ctrl + Left Ctrl","Left Shift + Unknown"})
            Check(normalize.Invoke(null,new object[]{invalid})==null,"Invalid chord is rejected "+invalid);
        Check((string?)normalize.Invoke(null,new object[]{"Left Ctrl + Keypad +"})=="Left Ctrl + Keypad +","Normalization preserves keypad plus");
        var modifiers=new[]{Key.LeftShift,Key.RightShift,Key.LeftCtrl,Key.RightCtrl,Key.LeftAlt,Key.RightAlt};
        ClickBinding(strong,"keyboard");foreach(var key in modifiers)SendCaptureKey(key,hold:true);SendCaptureKey(Key.Add);
        ClickBinding(strong,"mouse");foreach(var key in modifiers)SendCaptureKey(key,hold:true);SendCaptureMouse(MouseButton.XButton2);
        var fullChord=strong.Binding;Check(fullChord.Length>120,"Both slots can use all six modifiers");
        Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");Pump();strong=BindingRow("strong_attack");
        Check(strong.Binding==fullChord,"Long chords survive INI and custom-profile persistence");
        // Exercise the longer translated chord instructions in the smallest window.
        Select("UiLanguageCombo","vi");window.MinWidth=640;window.MinHeight=480;
        ClickBinding(strong,"mouse");SendCaptureKey(Key.LeftCtrl,hold:true);SendCaptureKey(Key.RightShift,hold:true);
        Render(Path.Combine(output,"controls-chord-small-vi.png"),640,480);
        Check(Visuals(Get<Grid>("CaptureOverlay")).OfType<Button>().Where(b=>b.IsVisible).All(b=>FitsWithin(b,Get<Grid>("CaptureOverlay"))),"Chord capture commands fit 640x480 Vietnamese layout");
        Call("CancelCapture_Click",window,new RoutedEventArgs());
        Select("UiLanguageCombo","en");window.MinWidth=1000;window.MinHeight=680;
        Select("BindingPresetCombo","default");Call("SaveSettings");
    }
    static void VerifyCapture(string output,string root,string? sdlPath) {
        ClickNav("Controller");
        Check(Get<DataGrid>("KeyBindingsGrid").IsReadOnly,"Bindings cannot be mistyped in the table");
        Check(Tag("BindingPresetCombo")=="default","Default preset detection");
        Check(!Get<ComboBoxItem>("CustomBindingPreset").IsEnabled,"Custom is unavailable until a personal binding exists");
        Select("BindingPresetCombo","ds3"); Pump();
        Check(BindingRow("lock_on").Binding=="Q, Mouse3" && BindingRow("transform").Binding=="C" && BindingRow("switch_item").Binding=="Down, WheelDown","Bloodborne adaptation of DS3 preset");
        Call("SaveSettings"); Call("LoadState"); Call("ApplyStateToUi"); Pump();
        Check(Tag("BindingPresetCombo")=="ds3" && BindingRow("lock_on").MouseToken=="Mouse3","DS3 bindings survive save/reload");
        Select("BindingPresetCombo","default");
        var attack=BindingRow("attack");
        ClickBinding(attack,"keyboard");
        Check(Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible,"Actual binding button starts capture");
        Render(Path.Combine(output,"controls-capture-key-en.png"));
        SendCaptureKey(Key.OemPlus);
        Check(attack.Binding=="=, Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Equals key maps to SDL and preserves mouse: "+attack.Binding);
        Check(Tag("BindingPresetCombo")=="custom","Editing marks the preset custom");
        Call("SaveSettings");
        Check(File.ReadAllLines(Path.Combine(root,"bbport.ini")).Contains("bind.attack==, Mouse1"),"Equals survives the INI separator");
        Call("LoadState"); Call("ApplyStateToUi"); Pump(); attack=BindingRow("attack");
        Check(attack.KeyboardToken=="=" && attack.MouseToken=="Mouse1","Two slots reload independently");
        Select("BindingPresetCombo","ds3"); Call("SaveSettings"); Call("LoadState"); Call("ApplyStateToUi");
        Select("BindingPresetCombo","custom"); Pump(); attack=BindingRow("attack");
        Check(attack.Binding=="=, Mouse1" && Tag("BindingPresetCombo")=="custom","Custom profile survives switching presets and restarting");
        ClickBinding(attack,"keyboard"); SendCaptureKey(Key.Escape);
        Check(attack.Binding=="=, Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Esc cancels without assigning or changing another slot");
        ClickBinding(attack,"keyboard"); SendCaptureKey(Key.OemComma);
        Check(attack.Binding=="=, Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible,"Comma separator is rejected without losing the binding");
        SendCaptureKey(Key.LeftShift); Check(attack.Binding=="Left Shift, Mouse1","Modifier key maps to SDL");
        ClickBinding(attack,"mouse"); SendCaptureKey(Key.Z);
        Check(attack.MouseToken=="Mouse1" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible,"Mouse capture cannot overwrite with keyboard input");
        SendCaptureMouse(MouseButton.XButton2);
        Check(attack.Binding=="Left Shift, Mouse5","Side mouse capture preserves keyboard");
        ClickBinding(attack,"mouse");
        var cancel=Visuals(Get<Grid>("CaptureOverlay")).OfType<Button>().Single(b=>b.Content?.ToString()=="Cancel");
        var cancelDown=new MouseButtonEventArgs(Mouse.PrimaryDevice,Environment.TickCount,MouseButton.Left) {RoutedEvent=Mouse.PreviewMouseDownEvent,Source=cancel};
        window.RaiseEvent(cancelDown); Pump();
        Check(!cancelDown.Handled && Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible,"Clicking a capture command does not assign left mouse");
        cancel.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)); Pump();
        Check(attack.Binding=="Left Shift, Mouse5" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Cancel button preserves both slots");
        ClickBinding(attack,"mouse");
        Render(Path.Combine(output,"controls-capture-mouse-en.png"));
        var wheel=new MouseWheelEventArgs(Mouse.PrimaryDevice,Environment.TickCount,-120) {RoutedEvent=Mouse.PreviewMouseWheelEvent};
        window.RaiseEvent(wheel); Pump();
        Check(wheel.Handled && attack.Binding=="Left Shift, WheelDown","Wheel capture consumes scrolling and preserves keyboard");
        ClickBinding(attack,"mouse");
        var wheelUp=new MouseWheelEventArgs(Mouse.PrimaryDevice,Environment.TickCount,120) {RoutedEvent=Mouse.PreviewMouseWheelEvent};
        window.RaiseEvent(wheelUp); Pump();
        Check(wheelUp.Handled && attack.Binding=="Left Shift, WheelUp","Wheel-up capture preserves the keyboard slot");
        foreach(var pair in new[]{(MouseButton.Left,"Mouse1"),(MouseButton.Right,"Mouse2"),(MouseButton.Middle,"Mouse3"),(MouseButton.XButton1,"Mouse4")}) {
            ClickBinding(attack,"mouse"); SendCaptureMouse(pair.Item1);
            Check(attack.MouseToken==pair.Item2,"Mouse mapping "+pair.Item1);
        }
        ClickBinding(attack,"keyboard",clear:true);
        Check(attack.Binding=="Mouse4","Clear keyboard leaves mouse assigned");
        ClickBinding(attack,"mouse",clear:true);
        Check(attack.Binding=="none","Clearing both slots serializes none");
        ClickBinding(attack,"keyboard"); Get<Button>("CaptureEscapeButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent)); Pump();
        Check(attack.Binding=="Escape","Explicit Esc button allows a menu binding");
        ClickBinding(attack,"keyboard");
        Call("InputCapture_Deactivated",window,EventArgs.Empty); Pump();
        Check(attack.Binding=="Escape" && Get<Grid>("CaptureOverlay").Visibility==Visibility.Collapsed,"Losing focus cancels capture without modifying keys");
        ClickBinding(attack,"keyboard"); SendCaptureKey(Key.W);
        Check(Get<TextBlock>("BindingConflictText").Visibility==Visibility.Visible && Get<TextBlock>("BindingConflictText").Text.Contains("Move forward"),"Shared keys warn without deleting another action");
        foreach(var group in new[]{("movement",7),("combat",7),("camera",5),("menu",10)}) {
            Select("BindingGroupCombo",group.Item1); Pump();
            Check(Get<DataGrid>("KeyBindingsGrid").Items.Count==group.Item2,"Action group "+group.Item1);
        }
        Select("BindingGroupCombo","all");
        Select("UiLanguageCombo","vi"); Pump();
        Check(attack.KeyboardToken=="W" && attack.Action=="Đánh" && BindingRow("transform").MouseDisplay=="Chuột giữa","Language switch preserves slots and translates labels");
        ClickBinding(attack,"mouse");
        Render(Path.Combine(output,"controls-capture-mouse-vi.png"));
        Check(Get<TextBlock>("CapturePrompt").Text.Contains("Nhấn một nút chuột"),"Vietnamese capture prompt");
        Call("CancelCapture_Click",window,new RoutedEventArgs());
        Select("UiLanguageCombo","en");
        Select("BindingPresetCombo","default"); Call("SaveSettings"); Pump();
        Check(Tag("BindingPresetCombo")=="default" && BindingRow("move_forward").Binding=="W" && BindingRow("attack").Binding=="Mouse1","Restore complete defaults after edits");
        Check(Get<DataGrid>("KeyBindingsGrid").Items.Cast<MainWindow.KeyBindingRow>().Take(4).Select(r=>r.Key).SequenceEqual(new[]{"move_forward","move_back","move_left","move_right"}),"Movement actions keep a stable order");
        var namesType=typeof(MainWindow).Assembly.GetType("BloodborneLauncher.InputKeyNames")!;
        var tokenMethod=namesType.GetMethod("Token",BindingFlags.Static|BindingFlags.NonPublic)!;
        Check((string?)tokenMethod.Invoke(null,new object[]{Key.D1,false})=="1" && (string?)tokenMethod.Invoke(null,new object[]{Key.NumPad1,false})=="Keypad 1","Number row and keypad are distinct");
        Check((string?)tokenMethod.Invoke(null,new object[]{Key.Return,true})=="Keypad Enter","Extended Enter maps to keypad Enter");
        attack=BindingRow("attack"); ClickBinding(attack,"keyboard");
        Call("InputCapture_WindowMessage",IntPtr.Zero,0x100,new IntPtr(13),new IntPtr(1L<<24),false);
        SendCaptureKey(Key.Return);
        Check(attack.Binding=="Keypad Enter, Mouse1","Window extended-key bit distinguishes keypad Enter during capture");
        ClickBinding(attack,"keyboard");
        Call("InputCapture_WindowMessage",IntPtr.Zero,0x100,new IntPtr(13),IntPtr.Zero,false);
        SendCaptureKey(Key.Return);
        Check(attack.Binding=="Return, Mouse1","Main Enter remains distinct during capture");
        Select("BindingPresetCombo","default"); Call("SaveSettings");
        Check(tokenMethod.Invoke(null,new object[]{Key.LWin,false})==null,"Windows shortcut is not offered as a game binding");
        if(!string.IsNullOrWhiteSpace(sdlPath)) {
            var library=NativeLibrary.Load(sdlPath);
            try {
                var scancode=Marshal.GetDelegateForFunctionPointer<SdlScancode>(NativeLibrary.GetExport(library,"SDL_GetScancodeFromName"));
                var tokens=Enum.GetValues<Key>().Select(k=>(string?)tokenMethod.Invoke(null,new object[]{k,false})).Where(t=>t!=null).Cast<string>().Append("Keypad Enter").Distinct();
                foreach(var token in tokens) Check(scancode(token)>0,"Runtime SDL accepts captured key "+token);
            } finally {NativeLibrary.Free(library);}
        }
        VerifyChords(output,root);
        Render(Path.Combine(output,"controls-ready-en.png"));
        Select("BindingGroupCombo","combat"); Render(Path.Combine(output,"controls-combat-en.png"));
        Select("UiLanguageCombo","vi"); Render(Path.Combine(output,"controls-combat-vi-compact.png"),1000,680);
        attack=BindingRow("attack"); Get<DataGrid>("KeyBindingsGrid").BringIntoView(); Pump();
        Check(FitsWithin(Get<DataGrid>("KeyBindingsGrid"),Get<ScrollViewer>("ControllerPage")),"Compact controller can show the complete binding table");
        ClickBinding(attack,"keyboard"); Render(Path.Combine(output,"controls-capture-200-percent.png"),900,520,192);
        Check(Visuals(Get<Grid>("CaptureOverlay")).OfType<Button>().Where(b=>b.IsVisible).All(b=>FitsWithin(b,Get<Grid>("CaptureOverlay"))),"Keyboard capture commands fit high-DPI layout");
        Call("CancelCapture_Click",window,new RoutedEventArgs());
        window.MinWidth=640;window.MinHeight=480;
        Render(Path.Combine(output,"controls-small-vi.png"),640,480);
        Get<DataGrid>("KeyBindingsGrid").BringIntoView(); Pump();
        Check(FitsWithin(Get<DataGrid>("KeyBindingsGrid"),Get<ScrollViewer>("ControllerPage")),"Small controller can show the complete binding table");
        Render(Path.Combine(output,"controls-small-bindings-vi.png"),640,480);
        attack=BindingRow("attack"); ClickBinding(attack,"mouse");
        Render(Path.Combine(output,"controls-capture-small-vi.png"),640,480);
        var captureButtons=Visuals(Get<Grid>("CaptureOverlay")).OfType<Button>().Where(b=>b.IsVisible).ToArray();
        Check(captureButtons.All(b=>FitsWithin(b,Get<Grid>("CaptureOverlay"))),"Capture commands fit the smallest window");
        Call("CancelCapture_Click",window,new RoutedEventArgs());
        Select("BindingGroupCombo","all"); Select("UiLanguageCombo","en");
        window.MinWidth=1000;window.MinHeight=680;
        Call("SaveSettings"); ClickNav("Home"); Render(Path.Combine(output,"launcher-home-before-checks.png"));
    }
    static bool FitsWithin(FrameworkElement child, FrameworkElement parent) {
        var bounds=child.TransformToAncestor(parent).TransformBounds(new Rect(0,0,child.ActualWidth,child.ActualHeight));
        return child.IsVisible && child.ActualWidth>0 && child.ActualHeight>0 && bounds.Left>=-1 && bounds.Top>=-1 && bounds.Right<=parent.ActualWidth+1 && bounds.Bottom<=parent.ActualHeight+1;
    }
    static void Render(string file, double width=1500, double height=920, double dpi=96) {
        window.Width=width; window.Height=height;
        window.UpdateLayout(); Pump();
        var element = (FrameworkElement)window.Content;
        element.Measure(new Size(width-2,height-2)); element.Arrange(new Rect(0,0,width-2,height-2)); element.UpdateLayout();
        var bitmap = new RenderTargetBitmap((int)((width-2)*dpi/96), (int)((height-2)*dpi/96), dpi, dpi, PixelFormats.Pbgra32);
        bitmap.Render(element);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = File.Create(file); encoder.Save(output);
    }
    [STAThread] static int Main(string[] args) {
        try { Run(args[0],args.ElementAtOrDefault(1)); return 0; } catch(Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    static void Run(string output,string? sdlPath) {
        Directory.CreateDirectory(output);
        var root = AppContext.BaseDirectory;
        string game = Path.Combine(root, "Test game");
        Directory.CreateDirectory(Path.Combine(game,"sce_sys"));
        File.WriteAllText(Path.Combine(game,"eboot.bin"),"launcher test fixture");
        File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),"01.09");
        File.WriteAllText(Path.Combine(root,"run.bat"),"@echo off\r\nset BB_ > launch-env.txt\r\necho %* > launch-args.txt\r\necho session>>launch-count.txt\r\nping 127.0.0.1 -n 3 > nul\r\n");
        File.WriteAllText(Path.Combine(root,"bbport.ini"),"# keep this comment\r\nunrelated_setting=keep\r\nupscaler=off\r\n");
        foreach (string key in Environment.GetEnvironmentVariables().Keys)
            if(key.StartsWith("BB_")) Environment.SetEnvironmentVariable(key,null);
        if (File.Exists(Path.Combine(root,"launcher-settings.json"))) File.Delete(Path.Combine(root,"launcher-settings.json"));
        foreach(var stale in new[]{"out/game_dir.txt","launch-env.txt","launch-args.txt"}) { var f=Path.Combine(root,stale); if(File.Exists(f)) File.Delete(f); }
        var app = new App();
        app.InitializeComponent();
        System.Globalization.CultureInfo.CurrentUICulture=new System.Globalization.CultureInfo("vi-VN");

        window = new MainWindow { ShowInTaskbar=false, ShowActivated=false, WindowStartupLocation=WindowStartupLocation.Manual, Left=-20000, Top=-20000 };
        window.Show(); Pump();
        Check(window.Title=="Techmo Bloodborne","Techmo window branding");
        Check(typeof(MainWindow).Assembly.GetName().Name=="Techmo.Launcher","Techmo executable identity");
        Check(Localization.Current.Language=="en","English is the default UI language");
        Check(Get<CheckBox>("OnlineEnabledCheck").Content?.ToString()=="Play online with shadNet","Default English UI is independent of Windows language");
        var uiLanguage=Get<ComboBox>("UiLanguageCombo");
        Check(uiLanguage.Items.Count==2,"Only English and Vietnamese UI choices");
        Check(uiLanguage.Items.Cast<ComboBoxItem>().Select(x=>x.Tag?.ToString()).SequenceEqual(new[]{"en","vi"}),"Supported UI language IDs");
        Check(Localization.Normalize("es")=="en" && Localization.Normalize("ru")=="en" && Localization.Normalize(null)=="en","Unsupported UI languages fall back to English");
        Check(!Get<Button>("PlayButton").IsEnabled, "Play must be disabled without a dump");
        Check(Get<TextBlock>("RuntimeStatus").Text=="Not built","Missing runtime status");
        Check(Get<TextBlock>("DlssStatus").Text=="Model DLL not installed","Missing DLSS status");
        Check(Get<TextBlock>("Fsr4Status").Text=="Assets not installed","Missing FSR4 status");
        Check(Get<Image>("HeroImage").Source is BitmapSource, "Embedded hero not loaded");
        Call("ConfigureGraphics_Click",Get<Button>("ConfigureGraphicsButton"),new RoutedEventArgs()); Pump();
        Check(Get<FrameworkElement>("GraphicsPage").Visibility==Visibility.Visible && Get<Button>("NavGraphics").Tag?.ToString()=="active","Home graphics shortcut opens and selects Graphics");
        Check(FitsWithin(Get<ComboBox>("UiLanguageCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Language selector reachable outside Home");
        ClickNav("Home");
        Get<CheckBox>("PcControlsCheck").IsChecked=false; Pump();
        Check(Get<TextBlock>("InputStatus").Text=="Controller","Home reflects gamepad input");
        Get<CheckBox>("PcControlsCheck").IsChecked=true; Pump();
        Check(Get<TextBlock>("InputStatus").Text=="Keyboard + mouse","Home reflects keyboard and mouse input");
        foreach(var name in new[]{"OnlineHostBox","OnlineWebApiBox","OnlineGameApiBox","OnlineWebsiteBox","OnlineUsernameBox","OnlineP2pPortBox"})
            Check(Get<TextBox>(name).Text=="","Online must have no preconfigured address/account: "+name);
        Check(Get<CheckBox>("OnlineEnabledCheck").IsChecked==false,"Online is opt-in");
        Render(Path.Combine(output,"launcher-play-unconfigured.png"));
        VerifyCapture(output,root,sdlPath);
        Get<TextBox>("PathBox").Text=game; Call("RefreshStatus");
        Check(!Get<Button>("PlayButton").IsEnabled,"Valid dump alone cannot launch a missing native runtime");
        Directory.CreateDirectory(Path.Combine(root,"out"));
        File.WriteAllText(Path.Combine(root,"out","bb-probe.exe"),"status fixture only");
        Call("RefreshStatus");
        Check(Get<Button>("PlayButton").IsEnabled,"Valid fixture should enable Play");
        Check(Get<TextBlock>("GameVersionStatus").Text.Contains("1.09"),"1.09 detection");
        File.Move(Path.Combine(game,"eboot.bin"),Path.Combine(game,"eboot.hidden"));
        Call("RefreshStatus"); Check(!Get<Button>("PlayButton").IsEnabled,"Missing eboot disables Play");
        File.Move(Path.Combine(game,"eboot.hidden"),Path.Combine(game,"eboot.bin"));
        Directory.Move(Path.Combine(game,"sce_sys"),Path.Combine(game,"sce_hidden"));
        Call("RefreshStatus"); Check(!Get<Button>("PlayButton").IsEnabled,"Missing sce_sys disables Play");
        Directory.Move(Path.Combine(game,"sce_hidden"),Path.Combine(game,"sce_sys"));
        foreach(var version in new[]{"01.08","01.00","unknown"}) {
            File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),version); Call("RefreshStatus");
            Check(Get<Button>("PlayButton").IsEnabled,"Preserve dump validation for "+version);
            Check(!Get<TextBlock>("GameVersionStatus").Text.Contains("1.09 ✓"),"No false 1.09 for "+version);
        }
        File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),"01.09");
        Directory.CreateDirectory(Path.Combine(root,"out"));
        File.WriteAllText(Path.Combine(root,"out","bb-probe.exe"),"status fixture only");
        var dlssPath=Path.Combine(root,"out","nvngx_dlss.dll");
        File.WriteAllText(dlssPath,"damaged model");
        Directory.CreateDirectory(Path.Combine(root,"fsr4_shaders")); Call("RefreshStatus");
        Check(Get<TextBlock>("RuntimeStatus").Text=="Ready","Runtime existence detection");
        Check(Get<TextBlock>("DlssStatus").Text=="Model DLL is invalid","Invalid DLSS is not installed");
        Check(Get<TextBlock>("Fsr4Status").Text=="Assets incomplete or damaged","Empty FSR4 directory is not installed");
        var pe=new byte[128];
        using(var writer=new BinaryWriter(new MemoryStream(pe))) {
            writer.Write((ushort)0x5A4D);writer.BaseStream.Position=0x3C;writer.Write((uint)64);
            writer.BaseStream.Position=64;writer.Write((uint)0x4550);writer.Write((ushort)0x8664);
            writer.BaseStream.Position=86;writer.Write((ushort)0x2000);writer.Write((ushort)0x20B);
        }
        File.WriteAllBytes(dlssPath,pe);
        var assetFolder=Path.Combine(root,"fsr4_shaders");
        var spv=new byte[20];BitConverter.GetBytes(0x07230203u).CopyTo(spv,0);
        foreach(var name in new[]{"rcas","spd_auto_exposure"})File.WriteAllBytes(Path.Combine(assetFolder,name+".spv"),spv);
        foreach(var model in new[]{"native","quality","balanced","performance","ultraperf","drs"}) {
            var prefix=Path.Combine(assetFolder,"fsr4_model_v07_i8_"+model);
            foreach(var suffix in new[]{"initializers","pre_weights"})File.WriteAllBytes(prefix+"_"+suffix+".bin",new byte[8]);
            File.WriteAllText(prefix+"_shader_manifest.json","{\"artifacts\":{}}");
            foreach(var tier in new[]{"1080","2160"}) {
                foreach(var name in new[]{"pre","post"})File.WriteAllBytes(prefix+"_"+tier+"_"+name+".spv",spv);
                for(int pass=1;pass<=12;pass++)File.WriteAllBytes(prefix+"_"+tier+"_pass"+pass+".spv",spv);
            }
        }
        Call("RefreshStatus");
        Check(Get<TextBlock>("DlssStatus").Text=="Model installed · driver checked at launch","x64 DLSS model detected");
        Check(Get<TextBlock>("Fsr4Status").Text=="v07 installed · experimental","Complete FSR4 set detected");
        var damagedPass=Path.Combine(assetFolder,"fsr4_model_v07_i8_quality_2160_pass12.spv");
        File.WriteAllBytes(damagedPass,new byte[20]);Call("RefreshStatus");
        Check(Get<TextBlock>("Fsr4Status").Text=="Assets incomplete or damaged","Damaged shader header rejected");
        File.WriteAllBytes(damagedPass,spv);Call("RefreshStatus");
        Select("UiLanguageCombo","vi");Pump();
        Check(Get<TextBlock>("Fsr4Status").Text=="Đã có v07 · thử nghiệm","Installed FSR4 status translates live");
        Select("UiLanguageCombo","en");
        Directory.CreateDirectory(Path.Combine(root,"fsr4_411")); Call("RefreshStatus");
        Check(Get<TextBlock>("Fsr4Status").Text=="4.1.1 available","FSR4 4.1.1 detection");
        File.Delete(Path.Combine(root,"out","bb-probe.exe")); File.Delete(Path.Combine(root,"out","nvngx_dlss.dll"));
        Directory.Delete(Path.Combine(root,"fsr4_shaders"),true); Directory.Delete(Path.Combine(root,"fsr4_411"));
        Get<TextBox>("PathBox").Text=Path.Combine(root,"bad"); Call("RefreshStatus");
        Check(!Get<Button>("PlayButton").IsEnabled,"Invalid path must disable Play");
        Get<TextBox>("PathBox").Text=game; Call("RefreshStatus");
        foreach(var pair in new[] { ("UpscalerCombo","QuickUpscaler"),("PresetCombo","QuickPreset"),("FpsCombo","QuickFps"),("ResolutionCombo","QuickResolution") }) {
            var detailed=Get<ComboBox>(pair.Item1); var quick=Get<ComboBox>(pair.Item2);
            for(int i=0;i<detailed.Items.Count;i++) { detailed.SelectedIndex=i; Check(quick.SelectedIndex==i,pair.Item1+" to quick "+i); }
            for(int i=quick.Items.Count-1;i>=0;i--) { quick.SelectedIndex=i; Check(detailed.SelectedIndex==i,pair.Item2+" to detailed "+i); }
        }
        Get<CheckBox>("FullscreenCheck").IsChecked=false; Check(Get<ComboBox>("QuickDisplay").SelectedIndex==1,"Display sync to quick");
        Get<ComboBox>("QuickDisplay").SelectedIndex=0; Check(Get<CheckBox>("FullscreenCheck").IsChecked==true,"Display sync from quick");
        Select("CameraFovCombo","1.25"); Select("CameraDistanceCombo","0.80"); Select("CameraHeightCombo","1.20");
        Select("GraphicsAoCombo","0.40"); Select("GraphicsShadowsCombo","1.40");
        Select("GraphicsBloomCombo","0.80"); Select("GraphicsSaturationCombo","1.20");
        Get<CheckBox>("RebirthCheck").IsChecked=true;
        Get<CheckBox>("GraphicsControlsCheck").IsChecked=true;
        Get<CheckBox>("GraphicsVignetteCheck").IsChecked=false;
        Select("UpscalerCombo","fsr411"); Select("PresetCombo","3"); Select("FpsCombo","90");
        Select("PresentCombo","FifoRelaxed"); Select("LiveResolutionCombo","auto"); Select("ModelLodCombo","-2");
        Get<ComboBox>("ResolutionCombo").SelectedIndex=2; Get<ComboBox>("ControllerCombo").SelectedIndex=2;
        foreach(var name in new[]{"CameraControlsCheck","ChangeAppearanceCheck","SharpenCheck","ShowFpsCheck","SsrCheck","SkipIntroCheck","DeveloperModeCheck","DetailedLogsCheck","FrameStatsCheck","GpuProfileCheck","AudioStatsCheck","FsrProfileCheck"}) Get<CheckBox>(name).IsChecked=true;
        foreach(var name in new[]{"ChromaticCheck","DofCheck","MotionBlurCheck","SsaoCheck","GameAaCheck","ShadowsCheck","ModsEnabledCheck"}) Get<CheckBox>(name).IsChecked=false;
        Check(Get<DataGrid>("KeyBindingsGrid").Items.Count==29,"All supported actions in launcher");
        Get<Slider>("MouseSensitivitySlider").Value=8;
        Get<CheckBox>("MouseInvertYCheck").IsChecked=true;
        var first=(MainWindow.KeyBindingRow)Get<DataGrid>("KeyBindingsGrid").Items[0];first.Binding="Z";
        Select("UiLanguageCombo","vi"); Pump();
        Check(Localization.Current.Language=="vi","Switch UI language without restarting");
        Check(first.Action=="Đi tới" && first.Binding=="Z","Translate action labels and preserve unsaved bindings");
        Check(Get<ComboBox>("FpsCombo").SelectedItem is ComboBoxItem selectedFps && selectedFps.Tag?.ToString()=="90","Language switch preserves unsaved graphics settings");
        Check(Get<TextBlock>("FooterMessage").Text=="Có thay đổi chưa lưu","Vietnamese transient status");
        Check(Get<CheckBox>("MouseCameraCheck").Content?.ToString()=="Điều khiển camera bằng chuột","Vietnamese PC controls");
        Check(Get<CheckBox>("OnlineEnabledCheck").Content?.ToString()=="Chơi trực tuyến qua shadNet","Vietnamese online page");
        Check(Get<DataGrid>("KeyBindingsGrid").Columns[0].Header?.ToString()=="Thao tác","Vietnamese table header");
        Select("PresetCombo","0");ClickNav("Home");Render(Path.Combine(output,"launcher-home-vi.png"));
        Render(Path.Combine(output,"launcher-home-vi-compact.png"),1100,720);Select("PresetCombo","3");
        foreach(var translatedPage in new[]{"Graphics","Performance","Mods","Advanced"}) {
            ClickNav(translatedPage);Render(Path.Combine(output,"launcher-"+translatedPage.ToLowerInvariant()+"-vi.png"));
        }
        ClickNav("General");Render(Path.Combine(output,"launcher-general-vi.png"));
        ClickNav("Controller");Render(Path.Combine(output,"launcher-controls-vi.png"));
        ClickNav("Online");Render(Path.Combine(output,"launcher-online-vi.png"));
        Call("SaveSettings");
        using(var languageState=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json"))))
            Check(languageState.RootElement.GetProperty("UiLanguage").GetString()=="vi","Persist Vietnamese UI preference");
        var legacyState=JsonNode.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json")))!;
        legacyState["Language"]="3";
        File.WriteAllText(Path.Combine(root,"launcher-settings.json"),legacyState.ToJsonString());
        Localization.Current.SetLanguage("en");Call("LoadState");Call("ApplyStateToUi");Pump();
        Check(Localization.Current.Language=="vi" && Get<CheckBox>("MouseCameraCheck").Content?.ToString()=="Điều khiển camera bằng chuột","Reload saved UI language");
        Select("UiLanguageCombo","en");Pump();
        Check(Get<CheckBox>("OnlineEnabledCheck").Content?.ToString()=="Play online with shadNet","Restore English throughout the UI");
        Check(Get<TextBlock>("FooterMessage").Text=="Unsaved changes","Restore English transient status");
        first=(MainWindow.KeyBindingRow)Get<DataGrid>("KeyBindingsGrid").Items[0];
        Check(first.Action=="Move forward" && first.Binding=="Z","English action labels keep bindings unchanged");
        ClickNav("Controller");Render(Path.Combine(output,"launcher-controls-en.png"));
        Call("SaveSettings");
        var unsupportedState=JsonNode.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json")))!;
        unsupportedState["UiLanguage"]="ru";
        File.WriteAllText(Path.Combine(root,"launcher-settings.json"),unsupportedState.ToJsonString());
        Call("LoadState");Call("ApplyStateToUi");Pump();
        Check(Localization.Current.Language=="en" && Get<ComboBox>("UiLanguageCombo").SelectedItem is ComboBoxItem fallback && fallback.Tag?.ToString()=="en","Migrate unsupported saved UI preference to English");
        Call("SaveSettings");
        string ini=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        foreach(var v in new[]{"widescreen=1","pc_controls=1","mouse_camera=1","mouse_menu=1","mouse_sensitivity=8","mouse_invert_y=1","bind.move_forward=Z","bind.attack=Mouse1"})Check(ini.Split("\r\n").Contains(v),"PC input/wide INI "+v);
        foreach(var expected in new[]{"rebirth=1","graphics_controls=1","graphics_vignette=0","graphics_ao_strength=0.40","graphics_shadow_scale=1.40","graphics_bloom=0.80","graphics_saturation=1.20"})
            Check(ini.Split("\r\n").Contains(expected),"Native engine option "+expected);
        foreach(var expected in new[]{"# keep this comment","unrelated_setting=keep","upscaler=fsr411","preset=3","sharpen=1","show_fps=1","output_res=2560x1440","fullscreen=1","live_resolution=auto","model_lod=-2","effect_chromatic_aberration=0","effect_dof=0","effect_motion_blur=0","effect_ssao=0","effect_game_aa=0","effect_dynamic_shadows=0","effect_ssr=1","skip_intro=1","change_appearance=1","camera_controls=1","camera_fov_scale=1.25","camera_distance_scale=0.80","camera_height_scale=1.20"}) Check(ini.Split("\r\n").Contains(expected),"INI "+expected);
        using(var json=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json")))) {
            Check(json.RootElement.GetProperty("GamePath").GetString()==game,"JSON game path");
            Check(json.RootElement.GetProperty("GamepadIndex").GetInt32()==2,"JSON controller");
            Check(json.RootElement.GetProperty("Fps").GetString()=="90","JSON fps");
            Check(!json.RootElement.GetProperty("ModsEnabled").GetBoolean(),"JSON mods");
        }
        Check(File.ReadAllText(Path.Combine(root,"out","game_dir.txt"))==game,"Remembered path");
        Check(!Get<Button>("PlayButton").IsEnabled,"Missing native runtime disables Play even with a valid dump");
        File.WriteAllText(Path.Combine(root,"out","bb-probe.exe"),"synthetic launch fixture only");
        Call("RefreshStatusCore",false);
        Call("Play_Click", Get<Button>("PlayButton"),new RoutedEventArgs());
        Call("Play_Click", Get<Button>("PlayButton"),new RoutedEventArgs());
        var envPath=Path.Combine(root,"launch-env.txt");
        for(int i=0;i<100 && !File.Exists(envPath);i++) Thread.Sleep(50);
        Thread.Sleep(200);
        Check(File.ReadAllLines(Path.Combine(root,"launch-count.txt")).Length==1,"Play cannot start duplicate sessions");
        string env=File.ReadAllText(envPath);
        foreach(var expected in new[]{"BB_PREBUILT=1","BB_DATA_DIR="+root.TrimEnd(Path.DirectorySeparatorChar),"BB_CONFIG="+Path.Combine(root.TrimEnd(Path.DirectorySeparatorChar),"bbport.ini"),"BB_GAME_DIR="+game,"BB_FPS=90","BB_FPS_LIMIT=90","BB_VBLANK_HZ=90","BB_LANGUAGE=3","BB_GAMEPAD_INDEX=2","BB_MODS_ENABLED=0","BB_PRESENT_MODE=FifoRelaxed","BB_DLSS_LOG=1","BB_FRAME_STATS=1","BB_AUDIO_STATS=1","BB_FSR4_PROFILE=1"}) Check(env.Split("\r\n").Contains(expected),"Launch environment "+expected);
        Check(File.ReadAllText(Path.Combine(root,"launch-args.txt")).Contains("--game-dir \""+game+"\""),"Launch path quoting");
        for(int i=0;i<100 && !window.IsVisible;i++) { Thread.Sleep(50); Pump(); }
        Check(window.IsVisible,"Launcher returns after the synthetic game session exits");
        Check(Get<Button>("PlayButton").IsEnabled,"Play is ready again after the game exits");
        string sessionDir=env.Split("\r\n").Single(line=>line.StartsWith("BB_SESSION_DIR=")).Substring("BB_SESSION_DIR=".Length);
        Check(Path.GetDirectoryName(env.Split("\r\n").Single(line=>line.StartsWith("BB_LOG_FILE=")).Substring("BB_LOG_FILE=".Length))==sessionDir,"Each launch log belongs to its unique session directory");
        Check(env.Split("\r\n").Contains("BB_GPU_PROFILE=1"),"GPU profiler switch reaches runtime");
        Check(Get<Border>("LastSessionCard").Visibility==Visibility.Visible,"Session outcome shown on Home");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x0000008B\",\"outcome\":{\"category\":\"crash\",\"reason\":\"access_violation\"},\"performance\":{\"available\":true,\"average_fps\":59.5,\"one_percent_low_fps\":44.1,\"p99_frame_ms\":22.68}}");
        Call("ShowSessionReport",sessionDir,139);Pump();
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne stopped unexpectedly","Native crash is not reported as a normal close");
        Check(Get<TextBlock>("LastSessionDetail").Text.Contains("Memory access violation") && Get<TextBlock>("LastSessionDetail").Text.Contains("0x0000008B"),"Crash shows evidence and exit code");
        Check(Get<TextBlock>("LastSessionPerformance").Text.Contains("59.5") && Get<TextBlock>("LastSessionPerformance").Text.Contains("44.1"),"Measured FPS and 1% low displayed");
        Select("UiLanguageCombo","vi");Pump();
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne dừng bất thường" && Get<TextBlock>("LastSessionDetail").Text.Contains("Lỗi truy cập bộ nhớ"),"Existing crash result translates to Vietnamese");
        ClickNav("Home");Render(Path.Combine(output,"session-crash-vi.png"));
        Select("UiLanguageCombo","en");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x00000001\",\"outcome\":{\"category\":\"startup_error\",\"reason\":\"preparation_failed\"}}");
        Call("ShowSessionReport",sessionDir,1);Pump();
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne could not start" && Get<TextBlock>("LastSessionDetail").Text.Contains("preparation failed"),"Preparation failure is distinguished from a native crash");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"running\"}");
        Call("ShowSessionReport",sessionDir,0);Pump();
        Check(Get<TextBlock>("LastSessionDetail").Text.Contains("before its report was completed"),"Incomplete report is not silently classified as a successful session");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"truncated");
        Call("ShowSessionReport",sessionDir,139);Pump();
        Check(Get<TextBlock>("LastSessionPerformance").Text.Contains("could not be read"),"Truncated crash report has a usable fallback");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x00000000\",\"outcome\":{\"category\":\"normal\",\"reason\":\"normal_exit\"}}");
        Call("ShowSessionReport",sessionDir,0);Pump();
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne closed","A successful runtime exit retains the normal close message");
        File.WriteAllText(Path.Combine(root,"run.bat"),"@echo off\r\nset BB_ > launch-env.txt\r\n> \"%BB_SESSION_DIR%\\session.json\" echo {\"status\":\"finished\",\"exit_code_hex\":\"0x0000008B\",\"outcome\":{\"category\":\"crash\",\"reason\":\"access_violation\"}}\r\nping 127.0.0.1 -n 2 > nul\r\nexit /b 139\r\n");
        Get<CheckBox>("DeveloperModeCheck").IsChecked=false;
        Environment.SetEnvironmentVariable("BB_GPU_PROFILE","1");
        Get<CheckBox>("GpuProfileCheck").IsChecked=false;
        Call("Play_Click", Get<Button>("PlayButton"),new RoutedEventArgs());
        for(int i=0;i<120 && !window.IsVisible;i++) { Thread.Sleep(50); Pump(); }
        Check(window.IsVisible && Get<Button>("PlayButton").IsEnabled,"Launcher recovers after a real synthetic nonzero process exit");
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne stopped unexpectedly" && Get<TextBlock>("LastSessionDetail").Text.Contains("Memory access violation"),"Exited handler reads the native outcome and shell exit code");
        string diagnosticEnv=File.ReadAllText(envPath);
        Check(diagnosticEnv.Split("\r\n").Contains("BB_FRAME_STATS=1"),"Frame recording works without Developer Mode");
        Check(!diagnosticEnv.Split("\r\n").Contains("BB_GPU_PROFILE=1"),"Disabled GPU profiling overrides an inherited profiler switch");
        Environment.SetEnvironmentVariable("BB_GPU_PROFILE",null);
        Check(!diagnosticEnv.Contains("BB_SESSION_DIR="+sessionDir+"\r\n"),"Consecutive sessions have different report directories");
        Call("ShowSessionReport",sessionDir,0);Get<ScrollViewer>("HomePage").ScrollToTop();
        Render(Path.Combine(output,"launcher-play.png"));
        foreach(var page in new[]{"General","Graphics","Performance","Controller","Online","Mods","Advanced"}) {
            ClickNav(page); Pump();
            Check(Get<FrameworkElement>(page+"Page").Visibility==Visibility.Visible,"Navigation "+page);
            Check(Get<FrameworkElement>("HomePage").Visibility==Visibility.Collapsed,"Home hidden "+page);
            Render(Path.Combine(output,"launcher-"+page.ToLowerInvariant()+".png"));
            Check(FitsWithin(Get<ComboBox>("UiLanguageCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Global language selector on "+page);
        }
        ClickNav("Graphics");
        var upscaler = Get<ComboBox>("UpscalerCombo");
        upscaler.IsDropDownOpen = true; Pump(); window.UpdateLayout();
        var popup = (System.Windows.Controls.Primitives.Popup)upscaler.Template.FindName("PART_Popup",upscaler);
        Check(popup.IsOpen && popup.Child != null,"Styled dropdown opens");
        Check(((FrameworkElement)popup.Child!).ActualWidth >= upscaler.ActualWidth,"Dropdown fits its selector");
        var menu = (FrameworkElement)popup.Child!;
        var menuBitmap = new RenderTargetBitmap((int)Math.Ceiling(menu.ActualWidth),(int)Math.Ceiling(menu.ActualHeight),96,96,PixelFormats.Pbgra32);
        menuBitmap.Render(menu);
        var menuEncoder = new PngBitmapEncoder(); menuEncoder.Frames.Add(BitmapFrame.Create(menuBitmap));
        using(var menuOutput=File.Create(Path.Combine(output,"launcher-dropdown.png"))) menuEncoder.Save(menuOutput);
        upscaler.IsDropDownOpen=false; Pump();
        ClickNav("Home"); Render(Path.Combine(output,"launcher-compact.png"),1000,680);
        Get<Button>("PlayButton").BringIntoView(); Pump();
        Check(FitsWithin(Get<Button>("PlayButton"),Get<ScrollViewer>("HomePage")),"Compact home can reach the entire Play button");
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        Render(Path.Combine(output,"launcher-compact-scrolled.png"),1000,680);
        Check(Get<ScrollViewer>("HomePage").VerticalOffset>0,"Compact home scrolls to quick controls");
        Check(FitsWithin(Get<ComboBox>("QuickFps"),Get<ScrollViewer>("HomePage")),"Compact home can reach quick FPS");
        Get<Expander>("SystemDetails").IsExpanded=true; Pump();
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        Check(FitsWithin(Get<TextBlock>("Fsr4Status"),Get<ScrollViewer>("HomePage")),"System capability details remain reachable");
        Render(Path.Combine(output,"launcher-system-details-compact.png"),1000,680);
        Get<Expander>("SystemDetails").IsExpanded=false;
        Get<ScrollViewer>("HomePage").ScrollToTop(); Pump();
        ClickNav("Graphics"); Render(Path.Combine(output,"launcher-graphics-compact.png"),1000,680);
        ClickNav("Home"); Render(Path.Combine(output,"launcher-150-percent.png"),1100,720,144);
        window.MinWidth=900; window.MinHeight=520;
        Render(Path.Combine(output,"launcher-200-percent.png"),900,520,192);
        Check(FitsWithin(Get<ComboBox>("UiLanguageCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Language selector fits the small high-DPI layout");
        Get<Button>("PlayButton").BringIntoView(); Pump();
        Check(FitsWithin(Get<Button>("PlayButton"),Get<ScrollViewer>("HomePage")),"High-DPI home can reach Play");
        Render(Path.Combine(output,"launcher-play-200-percent.png"),900,520,192);
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        Check(FitsWithin(Get<ComboBox>("QuickFps"),Get<ScrollViewer>("HomePage")),"High-DPI home can reach quick settings");
        Render(Path.Combine(output,"launcher-quick-200-percent.png"),900,520,192);
        Get<ScrollViewer>("SidebarNavigation").ScrollToBottom(); Pump();
        Check(Get<ScrollViewer>("SidebarNavigation").VerticalOffset>0,"Short-screen sidebar remains scrollable");
        ClickNav("Advanced");
        Check(Get<FrameworkElement>("AdvancedPage").Visibility==Visibility.Visible,"Advanced reachable on short screen");
        Render(Path.Combine(output,"launcher-advanced-200-percent.png"),900,520,192);
        window.MinWidth=640; window.MinHeight=480;
        ClickNav("Home");
        Select("UiLanguageCombo","vi");
        Render(Path.Combine(output,"launcher-small-vi.png"),640,480);
        Check(FitsWithin(Get<ComboBox>("UiLanguageCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Vietnamese language selector fits the minimum work area");
        Get<Button>("PlayButton").BringIntoView(); Pump();
        Check(FitsWithin(Get<Button>("PlayButton"),Get<ScrollViewer>("HomePage")),"Play remains reachable in the minimum work area");
        Render(Path.Combine(output,"launcher-small-play-vi.png"),640,480);
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        Check(FitsWithin(Get<ComboBox>("QuickFps"),Get<ScrollViewer>("HomePage")),"Quick settings remain reachable in the minimum work area");
        Render(Path.Combine(output,"launcher-small-quick-vi.png"),640,480);
        Select("UiLanguageCombo","en");
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("UpscalerCombo")=="fsr411" && Tag("FpsCombo")=="90" && Tag("PresetCombo")=="3","Settings reload");
        Check(Get<ComboBox>("QuickUpscaler").SelectedIndex==1 && Get<ComboBox>("QuickFps").SelectedIndex==2,"Quick settings reload");
        Check(Get<ComboBox>("FpsCombo").Items.Count==3 && Get<ComboBox>("QuickFps").Items.Count==3,"Only supported FPS presets");
        foreach(string legacy in new[]{"uncap","Unlimited","0","480","bad"}) {
            string saved=File.ReadAllText(Path.Combine(root,"launcher-settings.json"));
            File.WriteAllText(Path.Combine(root,"launcher-settings.json"),saved.Replace("\"Fps\": \"90\"","\"Fps\": \""+legacy+"\""));
            Call("LoadState"); Call("ApplyStateToUi");
            Check(Tag("FpsCombo")=="60" && Get<ComboBox>("QuickFps").SelectedIndex==1,"Legacy FPS fallback "+legacy);
            Call("SaveSettings");
            using(var migrated=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json"))))
                Check(migrated.RootElement.GetProperty("Fps").GetString()=="60","Persist FPS migration "+legacy);
            File.WriteAllText(Path.Combine(root,"launcher-settings.json"),saved);
        }
        Check(Get<CheckBox>("ChangeAppearanceCheck").IsChecked==true,"Mirror option reload");
        string mirrorIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),mirrorIni.Replace("change_appearance=1","change_appearance=0"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Get<CheckBox>("ChangeAppearanceCheck").IsChecked==false,"Runtime INI owns mirror setting");
        Check(Tag("CameraFovCombo")=="1.25" && Tag("CameraDistanceCombo")=="0.80" && Tag("CameraHeightCombo")=="1.20", "Camera runtime settings reload");
        string cameraIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),cameraIni.Replace("camera_fov_scale=1.25","camera_fov_scale=1.23"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("CameraFovCombo")=="1.23","Preserve in-game intermediate FOV");
        Check(Get<CheckBox>("RebirthCheck").IsChecked==true && Get<CheckBox>("GraphicsControlsCheck").IsChecked==true,"Enhancements reload from INI");
        Check(Tag("GraphicsAoCombo")=="0.40" && Tag("GraphicsShadowsCombo")=="1.40" && Tag("GraphicsBloomCombo")=="0.80" && Tag("GraphicsSaturationCombo")=="1.20","Live graphics values reload");
        string runtimeIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),runtimeIni.Replace("graphics_bloom=0.80","graphics_bloom=0.60").Replace("rebirth=1","rebirth=0").Replace("effect_ssao=0","effect_ssao=1"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("GraphicsBloomCombo")=="0.60" && Get<CheckBox>("RebirthCheck").IsChecked==false && Get<CheckBox>("SsaoCheck").IsChecked==true,"Native pages remain authoritative over stale launcher JSON");
        Get<TextBox>("OnlineHostBox").Text="server.example";
        Get<TextBox>("OnlinePortBox").Text="31413";
        Get<TextBox>("OnlineWebApiBox").Text="http://server.example:31415";
        Get<TextBox>("OnlineGameApiBox").Text="https://game.example:34443";
        Get<TextBox>("OnlineWebsiteBox").Text="https://accounts.example:34416";
        Get<TextBox>("OnlineUsernameBox").Text="TestHunter";
        Get<TextBox>("OnlineP2pPortBox").Text="32000";
        Get<PasswordBox>("OnlinePasswordBox").Password="SYNTHETIC-SECRET";
        Get<PasswordBox>("OnlineTokenBox").Password="SYNTHETIC-TOKEN";
        foreach(var name in new[]{"OnlineEnabledCheck","OnlineConnectedCheck","OnlineRememberCheck"}) Get<CheckBox>(name).IsChecked=true;
        Call("SaveSettings");
        string onlineIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        foreach(var expected in new[]{"online_enabled=1","online_host=server.example","online_port=31413","online_webapi=http://server.example:31415","online_game_api=https://game.example:34443","online_website=https://accounts.example:34416","online_p2p_port=32000","online_username=TestHunter"})
            Check(onlineIni.Split("\r\n").Contains(expected),"Online INI "+expected);
        foreach(var path in new[]{"bbport.ini","launcher-settings.json"})
            Check(!File.ReadAllText(Path.Combine(root,path)).Contains("SYNTHETIC-"),"Secrets excluded from settings: "+path);
        var protectedAccount=Directory.GetFiles(Path.Combine(root,"user","accounts"),"*.bin").Single();
        Check(!System.Text.Encoding.UTF8.GetString(File.ReadAllBytes(protectedAccount)).Contains("SYNTHETIC-SECRET"),"Windows-protected account on disk");
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Get<PasswordBox>("OnlinePasswordBox").Password=="SYNTHETIC-SECRET" && Get<PasswordBox>("OnlineTokenBox").Password=="SYNTHETIC-TOKEN","Protected account reload");
        Check(Get<TextBox>("OnlineWebApiBox").Text=="http://server.example:31415","Server port survives reload");
        var start=new System.Diagnostics.ProcessStartInfo();
        start.Environment["SHADPS4_HTTP_HOST_OVERRIDES_JSON"]="unrelated-app.json";
        Call("ConfigureOnlineLaunch",start);
        Check(start.Environment["BB_ONLINE"]=="1" && start.Environment["BB_SHADNET_SERVER"]=="server.example:31413","Explicit selected server environment");
        Check(start.Environment["BB_SHADNET_PASSWORD"]=="SYNTHETIC-SECRET" && start.Environment["SHADPS4_P2P_PORT"]=="32000","Account and selected P2P port reach child");
        Check(!start.Environment.ContainsKey("SHADPS4_HTTP_HOST_OVERRIDES_JSON"),"Python must generate our own override");
        Get<TextBox>("OnlineP2pPortBox").Text="65536";
        bool onlineSaved=(bool)typeof(MainWindow).GetMethod("SaveSettings",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        Check(!onlineSaved,"Bad online port does not crash or launch");
        Get<TextBox>("OnlineP2pPortBox").Text="32000";
        Get<CheckBox>("OnlineEnabledCheck").IsChecked=false;
        Get<CheckBox>("OnlineRememberCheck").IsChecked=false;
        Call("SaveSettings");
        Call("ConfigureOnlineLaunch",start);
        Check(start.Environment["BB_ONLINE"]=="0" && !start.Environment.ContainsKey("BB_SHADNET_PASSWORD") && !start.Environment.ContainsKey("SHADPS4_P2P_PORT"),"Offline removes inherited online credentials and ports");
        Check(!File.Exists(protectedAccount),"Unchecking remember forgets account");
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Get<PasswordBox>("OnlinePasswordBox").Password.Length==0,"Forgotten account is not restored");
        Console.WriteLine($"PASS: {assertions} assertions; settings, navigation, all quick options, launch environment and artwork.");
        window.Close(); app.Shutdown();
    }
}




'@
Set-Content -Encoding utf8 -LiteralPath (Join-Path $verificationRoot 'Program.cs') -Value $program
$arguments = @('run','--project',(Join-Path $verificationRoot 'LauncherChecks.csproj'),'--configuration','Release')
if ($PackageCache) { $arguments += "--property:RestorePackagesPath=$([IO.Path]::GetFullPath($PackageCache))" }
if ($SdlDll) { & dotnet @arguments -- $PreviewDirectory ([IO.Path]::GetFullPath($SdlDll)) }
else { & dotnet @arguments -- $PreviewDirectory }
if ($LASTEXITCODE -ne 0) { throw 'Launcher verification failed.' }
Write-Host "Preview images: $PreviewDirectory"
Write-Host "Isolated test fixtures: $verificationRoot"
