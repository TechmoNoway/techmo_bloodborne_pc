param([string]$PreviewDirectory, [string]$PackageCache, [string]$SdlDll, [switch]$PublicRelease)
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
using System.IO.Compression;
using System.Security.Cryptography;
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
    static string sourceRoot = Environment.GetEnvironmentVariable("TECHMO_VERIFY_SOURCE_ROOT")!;
    static int assertions;
    static bool publicRelease = __PUBLIC_RELEASE__;
    static T Get<T>(string name) where T : class => (T)window.FindName(name);
    static void Call(string name, params object[] args) => typeof(MainWindow).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(window, args);
    static void Check(bool value, string message) { if (!value) throw new Exception(message); assertions++; }
    static void CheckSessionCard(string context) {
        foreach (string name in new[]{"LastSessionCard","HomeSessionCard"}) {
            var card = Get<Border>(name);
            Check(card.Visibility == (publicRelease ? Visibility.Collapsed : Visibility.Visible),
                context + " " + name + (publicRelease ? ": public release hides last session" : ": Actions build shows last session"));
            if (publicRelease) Check(!card.IsVisible && card.ActualHeight == 0, context + " " + name + ": hidden card takes no layout space");
        }
    }
    static void CheckDiagnosticsAccess(string context) {
        if (publicRelease) {
            Check(!Get<Button>("ViewDiagnosticsButton").IsVisible && Get<Border>("HomeSessionCard").Visibility==Visibility.Collapsed,
                context + ": public release omits the session summary shortcut");
            Get<Button>("NavAdvanced").BringIntoView(); Pump();
            Check(FitsWithin(Get<Button>("NavAdvanced"),Get<ScrollViewer>("SidebarNavigation")),
                context + ": Diagnostics remains reachable in the sidebar");
        } else Check(FitsWithin(Get<Button>("ViewDiagnosticsButton"),Get<ScrollViewer>("HomePage")),
            context + ": Actions build can reach the session Diagnostics shortcut");
    }
    static string Tag(string name) => window.FindName(name) is Slider slider
        ? (Math.Round(slider.Value,MidpointRounding.AwayFromZero)/100).ToString("0.00",System.Globalization.CultureInfo.InvariantCulture)
        : ((ComboBoxItem)Get<ComboBox>(name).SelectedItem).Tag?.ToString() ?? "";
    static void Select(string name, string tag) {
        if (window.FindName(name) is Slider slider) { slider.Value=double.Parse(tag,System.Globalization.CultureInfo.InvariantCulture)*100;return; }
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
    static void VerifyUpscalingAvailability(string output,string root) {
        Select("UpscalerCombo","dlss"); Select("PresetCombo","3");
        foreach(string provider in new[]{"off","taa"}) {
            Select("UpscalerCombo",provider);
            foreach(string language in new[]{"en","vi"}) {
                Select("UiLanguageCombo",language); Pump();
                string disabled=language=="en"?"Disabled":"Không áp dụng";
                foreach(string name in new[]{"PresetCombo"}) {
                    var box=Get<ComboBox>(name); box.ApplyTemplate();
                    var placeholder=(TextBlock)box.Template.FindName("DisabledSelection",box);
                    Check(!box.IsEnabled && !box.IsDropDownOpen,provider+" disables "+name);
                    Check(placeholder.Visibility==Visibility.Visible && placeholder.Text==disabled,"Disabled placeholder translates without replacing the selection: "+name);
                    Check(box.SelectedIndex==3,provider+" preserves the previous Performance choice: "+name);
                    Check(System.Windows.Automation.AutomationProperties.GetHelpText(box)==Get<TextBlock>("PresetUnavailableHint").Text,"Accessible explanation matches the provider: "+name);
                }
                Check(Get<TextBlock>("PresetUnavailableHint").Text.Contains(provider=="taa"?"TAA":language=="en"?"off":"Đã tắt"),"Provider-specific disabled explanation");
                ClickNav("Graphics"); Render(Path.Combine(output,"quality-disabled-"+provider+"-"+language+".png"),1440,900);
                if(provider=="taa") { ClickNav("Home"); Render(Path.Combine(output,"quality-disabled-home-"+language+".png"),1440,900); }
            }
            Call("SaveSettings"); Call("LoadState"); Call("ApplyStateToUi"); Pump();
            Check(!Get<ComboBox>("PresetCombo").IsEnabled && Tag("PresetCombo")=="3","Disabled state survives reload and retains the real preference");
            Check(File.ReadAllLines(Path.Combine(root,"bbport.ini")).Contains("preset=3"),"Disabled is never written as a runtime preset");
            Select("UpscalerCombo","dlss");
        }
        foreach(string provider in new[]{"dlss","fsr3","fsr4","fsr411"}) {
            Select("UpscalerCombo",provider); Pump();
            Check(Get<ComboBox>("PresetCombo").IsEnabled && Tag("PresetCombo")=="3",provider+" restores Performance");
            Check(Get<TextBlock>("PresetUnavailableHint").Visibility==Visibility.Collapsed,"Active upscaler hides the disabled explanation");
        }
        Select("PresetCombo","0"); Select("UpscalerCombo","off"); Select("UpscalerCombo","taa"); Select("UpscalerCombo","dlss");
        Check(Tag("PresetCombo")=="0","Native AA also survives Off and TAA transitions");
        Select("UiLanguageCombo","en"); Select("UpscalerCombo","fsr411"); Select("PresetCombo","3");
    }
    static void VerifySavesAndHdr(string output,string root) {
        var type=typeof(MainWindow).Assembly.GetType("BloodborneLauncher.SaveDataStore")!;
        object Store(string path,Func<bool>? running=null) => Activator.CreateInstance(type,path,running ?? new Func<bool>(()=>false))!;
        object? Invoke(object store,string name,params object[] args) {
            try { return type.GetMethod(name)!.Invoke(store,args); }
            catch(TargetInvocationException ex) { throw ex.InnerException!; }
        }
        string Prop(object item,string key)=>(string)item.GetType().GetProperty(key)!.GetValue(item)!;
        int Count(object store,string name)=>((System.Collections.ICollection)Invoke(store,name)!).Count;
        bool Rejected(object store,string method,params object[] args) {
            try { Invoke(store,method,args); return false; }
            catch(Exception e) when(e is InvalidDataException or IOException or JsonException) { return true; }
        }
        string fixture=Path.Combine(root,"save-checks");
        string source=Path.Combine(fixture,"source");
        string title=Path.Combine(source,"savedata","1","CUSA03173");
        string memory=title+".memory";
        Directory.CreateDirectory(Path.Combine(title,"SPRJ0005"));
        Directory.CreateDirectory(Path.Combine(title,"SPRJ0005.sce_sys"));
        Directory.CreateDirectory(memory);
        File.WriteAllText(Path.Combine(title,"SPRJ0005","userdata0000"),"original character");
        File.WriteAllText(Path.Combine(title,"SPRJ0005.sce_sys","param.bin"),"metadata");
        File.WriteAllText(Path.Combine(memory,"memory.dat"),"original memory");
        var store=Store(source);
        Check(Count(store,"ListSaves")==1,"Save discovery groups title and memory with metadata");
        string zip=Prop(Invoke(store,"Backup","1","CUSA03173")!,"Path");
        Check(Count(store,"ListBackups")==1,"Created ZIP appears in backups");
        using(var archive=ZipFile.OpenRead(zip)) Check(archive.GetEntry("data/CUSA03173/SPRJ0005.sce_sys/param.bin")!=null && archive.GetEntry("data/CUSA03173.memory/memory.dat")!=null,"Backup includes metadata and memory");
        File.WriteAllText(Path.Combine(title,"SPRJ0005","userdata0000"),"new progress");
        File.WriteAllText(Path.Combine(title,"extra-file"),"must be removed on restore");
        File.WriteAllText(Path.Combine(memory,"memory.dat"),"new memory");
        string safety=(string)Invoke(store,"Restore",zip)!;
        Check(File.Exists(safety) && Count(store,"ListBackups")==2,"Restore makes a separate safety backup");
        Check(File.ReadAllText(Path.Combine(title,"SPRJ0005","userdata0000"))=="original character" && !File.Exists(Path.Combine(title,"extra-file")),"Restore replaces exactly the backed up save set");
        Check(File.ReadAllText(Path.Combine(memory,"memory.dat"))=="original memory","Restore replaces paired save memory");
        Invoke(store,"Restore",safety);
        Check(File.ReadAllText(Path.Combine(title,"SPRJ0005","userdata0000"))=="new progress" && File.ReadAllText(Path.Combine(memory,"memory.dat"))=="new memory","Safety backup recovers the previous progress and memory");
        string target=Path.Combine(fixture,"target");
        var fresh=Store(target);
        Invoke(fresh,"Restore",zip);
        Check(File.Exists(Path.Combine(target,"savedata","1","CUSA03173","SPRJ0005","userdata0000")),"Imported backup restores into a new user folder");
        string empty=Path.Combine(fixture,"empty");Directory.CreateDirectory(Path.Combine(empty,"savedata","1","CUSA03173"));
        Invoke(Store(empty),"Restore",zip);
        Check(File.Exists(Path.Combine(empty,"savedata","1","CUSA03173","SPRJ0005","userdata0000")),"Restore tolerates an empty existing title folder");
        var running=Store(source,()=>true);
        Check(Rejected(running,"Backup","1","CUSA03173") && Rejected(running,"Restore",zip),"Backup and restore are blocked while the game is running");
        Check(Rejected(store,"Backup","..","CUSA03173") && Rejected(store,"Backup","1","../other"),"Save operations reject invalid account/title paths");
        string MakeZip(string label,string name,string contents,string hash, bool extra=false) {
            string path=Path.Combine(fixture,label+".zip");
            using(var archive=ZipFile.Open(path,ZipArchiveMode.Create)) {
                using(var writer=new StreamWriter(archive.CreateEntry("data/"+name).Open())) writer.Write(contents);
                if(extra)using(var writer=new StreamWriter(archive.CreateEntry("unexpected").Open()))writer.Write("extra");
                using(var writer=new StreamWriter(archive.CreateEntry("manifest.json").Open()))
                    writer.Write(JsonSerializer.Serialize(new {Version=1,UserId="1",TitleId="CUSA03173",CreatedUtc=DateTime.UtcNow,Files=new[]{new{Name=name,Bytes=(long)System.Text.Encoding.UTF8.GetByteCount(contents),Sha256=hash}}}));
            } return path;
        }
        string hash=Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes("payload")));
        foreach(string name in new[]{"CUSA03173/../escape","../escape","CUSA03173/CON.txt","CUSA03173/trailing. ","CUSA03173/x:stream","CUSA03173/\\escape"}) {
            var hostile=MakeZip("bad-"+Guid.NewGuid().ToString("N"),name,"payload",hash);
            Check(Rejected(store,"Restore",hostile),"Reject unsafe ZIP entry "+name);
        }
        var tampered=MakeZip("tampered","CUSA03173/SPRJ0005/userdata0000","damaged",hash);
        string current=File.ReadAllText(Path.Combine(title,"SPRJ0005","userdata0000"));
        Check(Rejected(store,"Restore",tampered) && File.ReadAllText(Path.Combine(title,"SPRJ0005","userdata0000"))==current,"Integrity failure preserves live saves");
        var unexpected=MakeZip("unexpected","CUSA03173/SPRJ0005/userdata0000","payload",hash,true);
        Check(Rejected(store,"Restore",unexpected),"Unexpected ZIP files are rejected");
        var memoryOnly=MakeZip("memory-only","CUSA03173.memory/memory.dat","payload",hash);
        Check(Rejected(store,"Restore",memoryOnly),"Memory-only archives cannot replace full saves");
        Check(!Directory.EnumerateDirectories(Path.Combine(source,"savedata"),".techmo-*").Any(),"Save staging/rollback directories are cleaned up");
        // Fail the second directory swap after validation and safety backup, then verify rollback.
        FileStream? held=null; int probes=0;
        var failing=Store(source,()=> { if(++probes==5)held=new FileStream(Path.Combine(memory,"memory.dat"),FileMode.Open,FileAccess.Read,FileShare.Read);return false; });
        try { Check(Rejected(failing,"Restore",zip),"A locked memory directory aborts the replacement"); }
        finally { held?.Dispose(); }
        Check(File.ReadAllText(Path.Combine(title,"SPRJ0005","userdata0000"))==current && File.ReadAllText(Path.Combine(memory,"memory.dat"))=="new memory","A failed second swap rolls back the first save directory");
        Check(Rejected(store,"Restore",Path.Combine(fixture,"missing.zip")),"Missing import returns a handled error");

        Environment.SetEnvironmentVariable("BB_USER_DIR",source);
        Call("InitializeSaves");Environment.SetEnvironmentVariable("BB_USER_DIR",null);
        ClickNav("Mods");Pump();
        Check(Get<StackPanel>("SaveDataPanel").Visibility==Visibility.Visible && Get<ComboBox>("SaveSetCombo").Items.Count==1,"Save tab discovers isolated data");
        Check(Get<ComboBox>("SaveSetCombo").SelectedItem.ToString()!.StartsWith("CUSA03173 · User 1"),"Save picker displays a friendly label instead of record internals");
        Call("SaveSettings");Get<ComboBox>("SaveSetCombo").SelectedIndex=0;Call("RefreshSaveLists");
        Check(!(bool)typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!,"Selecting or refreshing saves does not dirty the profile");
        Render(Path.Combine(output,"control-hub-saves-en.png"));
        Select("UiLanguageCombo","vi");Render(Path.Combine(output,"control-hub-saves-vi.png"));
        Check(Get<Button>("BackupSaveButton").Content?.ToString()=="Tạo bản sao lưu","Save actions translate to Vietnamese");
        Check(Get<ComboBox>("SaveSetCombo").SelectedItem.ToString()!.Contains("Người dùng 1"),"Save picker label updates when the language changes");
        Check(Get<ComboBox>("SaveSetCombo").SelectionBoxItem.ToString()!.Contains("Người dùng 1"),"WPF selection text refreshes rather than retaining the English cached label");
        Select("UiLanguageCombo","en");
        Call("SavesTab_Click",Get<Button>("ModsTab"),new RoutedEventArgs());
        Check(Get<StackPanel>("ModSettingsPanel").Visibility==Visibility.Visible,"Existing mods remain in their own sub-tab");
        Call("SavesTab_Click",Get<Button>("SavesTab"),new RoutedEventArgs());
        ClickNav("Display");
        Check(Get<CheckBox>("HdrOutputCheck").IsChecked==false && !Get<ComboBox>("HdrWhiteCombo").IsEnabled,"HDR defaults to Off with calibration disabled");
        Get<CheckBox>("HdrOutputCheck").IsChecked=true;
        Check(Get<ComboBox>("HdrWhiteCombo").IsEnabled && Get<ComboBox>("HdrPeakCombo").IsEnabled,"Lantern HDR enables paper white and peak calibration");
        Get<CheckBox>("HdrExpandCheck").IsChecked=true;Select("HdrWhiteCombo","250");Select("HdrPeakCombo","800");Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
        Check(Get<ComboBox>("HdrPeakCombo").IsEnabled && Tag("HdrWhiteCombo")=="250" && Tag("HdrPeakCombo")=="800","HDR controls survive save/reload");
        var psi=new System.Diagnostics.ProcessStartInfo();psi.Environment["BB_HDR"]="1";psi.Environment["BB_HDR_OUTPUT"]="inherited";
        Call("ConfigureHdrLaunch",psi);
        Check(psi.Environment["BB_HDR_PAPER_WHITE"]=="250" && psi.Environment["BB_HDR_PEAK"]=="800" && psi.Environment["BB_HDR_MODE"]=="expand" && psi.Environment["BB_HDR"]=="1" && !psi.Environment.ContainsKey("BB_HDR_OUTPUT"),"HDR launch uses Lantern conversion settings and removes retired keys");
        Get<ScrollViewer>("DisplayPage").ScrollToBottom();Pump();Render(Path.Combine(output,"control-hub-hdr-en.png"));
        Select("UiLanguageCombo","vi");Render(Path.Combine(output,"control-hub-hdr-vi.png"));Select("UiLanguageCombo","en");
        Get<CheckBox>("HdrOutputCheck").IsChecked=false;Call("SaveSettings");Call("ConfigureHdrLaunch",psi);
        Check(psi.Environment["BB_HDR"]=="0" && !psi.Environment.ContainsKey("BB_HDR_OUTPUT"),"HDR Off clears inherited HDR activation");
        Check(((SolidColorBrush)Application.Current.Resources["AppBackgroundBrush"]).Color==Color.FromRgb(8,8,8) && ((SolidColorBrush)Application.Current.Resources["PanelBrush"]).Color.A<255,"Dark theme has black background with translucent panels");
    }

    static void VerifyOverlaySettings(string output,string root) {
        var fields=new[]{"ShowFpsCheck","ShowCpuCheck","ShowGpuCheck"};
        var keys=new[]{"show_fps","show_cpu","show_gpu"};
        string ini=Path.Combine(root,"bbport.ini"),library=Path.Combine(root,"user","launcher","profiles.json");
        for(int mask=0;mask<8;++mask) {
            for(int i=0;i<fields.Length;++i)Get<CheckBox>(fields[i]).IsChecked=(mask&(1<<i))!=0;
            Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
            Check(Enumerable.Range(0,3).All(i=>Get<CheckBox>(fields[i]).IsChecked==((mask&(1<<i))!=0) && File.ReadAllLines(ini).Contains(keys[i]+"="+((mask&(1<<i))!=0?"1":"0"))),"FPS/CPU/GPU checkboxes persist independently: "+mask);
        }
        var picker=Get<ComboBox>("ProfileCombo");picker.SelectedIndex=0;
        Get<CheckBox>("ShowCpuCheck").IsChecked=true;Get<CheckBox>("ShowGpuCheck").IsChecked=false;Call("SaveSettings");
        picker.SelectedIndex=1;Get<CheckBox>("ShowCpuCheck").IsChecked=false;Get<CheckBox>("ShowGpuCheck").IsChecked=true;Call("SaveSettings");
        picker.SelectedIndex=0;
        Check(Get<CheckBox>("ShowCpuCheck").IsChecked==true && Get<CheckBox>("ShowGpuCheck").IsChecked==false,"Profiles restore their own monitoring choices");
        Call("SaveSettings");picker.SelectedIndex=1;Call("SaveSettings");
        var legacy=JsonNode.Parse(File.ReadAllText(library))!;
        foreach(var profile in legacy["Profiles"]!.AsArray()) { profile!["Settings"]!.AsObject().Remove("ShowCpu");profile["Settings"]!.AsObject().Remove("ShowGpu"); }
        File.WriteAllText(library,legacy.ToJsonString());Call("InitializeProfiles");
        Call("ApplyProfile",Get<ComboBox>("ProfileCombo").SelectedItem);Call("SaveSettings");
        Check(Get<CheckBox>("ShowCpuCheck").IsChecked==false && Get<CheckBox>("ShowGpuCheck").IsChecked==false && JsonNode.Parse(File.ReadAllText(library))!["Version"]!.GetValue<int>()==1,"Existing v1 profiles migrate missing metrics to Off without losing the profile");
        string saved=File.ReadAllText(ini);
        File.WriteAllText(ini,saved.Replace("show_fps_cpu=0","show_fps_cpu=1").Replace("show_fps_gpu=0","show_fps_gpu=1"));
        Call("LoadState");Call("ApplyStateToUi");
        Check(Get<CheckBox>("ShowCpuCheck").IsChecked==true && Get<CheckBox>("ShowGpuCheck").IsChecked==true,"In-game monitoring changes are read back from the runtime INI");
        ClickNav("Display");Get<CheckBox>("ShowFpsCheck").IsChecked=true;
        Render(Path.Combine(output,"overlay-options-en.png"),1320,960);Get<ScrollViewer>("DisplayPage").ScrollToVerticalOffset(600);Pump();Render(Path.Combine(output,"overlay-options-en.png"),1320,960);
        Select("UiLanguageCombo","vi");Render(Path.Combine(output,"overlay-options-vi.png"),1320,960);
        Check(Get<CheckBox>("ShowCpuCheck").Content?.ToString()=="Hiện mức sử dụng CPU (%)" && Get<CheckBox>("ShowGpuCheck").Content?.ToString()=="Hiện mức sử dụng GPU (%)","Monitoring controls translate to Vietnamese");
        Select("UiLanguageCombo","en");Call("SaveSettings");
    }

    static void VerifySharpening(string output,string root) {
        string ini=Path.Combine(root,"bbport.ini"), library=Path.Combine(root,"user","launcher","profiles.json");
        var slider=Get<Slider>("SharpnessSlider");
        ClickNav("Graphics");Call("GraphicsTab_Click",Get<Button>("GraphicsUpscalingTab"),new RoutedEventArgs());
        Select("UpscalerCombo","dlss");Get<CheckBox>("SharpenCheck").IsChecked=true;
        foreach(double value in new[]{0,0.3,1,2}) {
            slider.Value=value;Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
            string expected=value.ToString("0.00",System.Globalization.CultureInfo.InvariantCulture);
            Check(slider.Value==value && Get<TextBlock>("SharpnessValue").Text==expected && File.ReadAllLines(ini).Contains("sharpness="+expected),"Sharpness reaches the native INI and survives restart: "+expected);
        }
        slider.Value=0.37;Get<CheckBox>("SharpenCheck").IsChecked=false;Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
        Check(!slider.IsEnabled && slider.Value==0.37 && File.ReadAllLines(ini).Contains("sharpen=0"),"Turning sharpening off preserves its chosen strength");
        Get<CheckBox>("SharpenCheck").IsChecked=true;
        foreach(string provider in new[]{"taa","dlss","fsr3","fsr4","fsr411"}) {
            Select("UpscalerCombo",provider);
            Check(slider.IsEnabled && Get<CheckBox>("SharpenCheck").IsEnabled && slider.Value==0.37,"Sharpness available for "+provider);
        }
        Select("UpscalerCombo","off");
        Check(!slider.IsEnabled && !Get<CheckBox>("SharpenCheck").IsEnabled && slider.Value==0.37,"Upscaler Off disables sharpening without erasing its value");
        Select("UpscalerCombo","dlss");
        slider.RaiseEvent(new KeyEventArgs(Keyboard.PrimaryDevice,PresentationSource.FromVisual(window),Environment.TickCount,Key.Right) {RoutedEvent=Keyboard.KeyDownEvent});
        Check(Math.Abs(slider.Value-0.38)<0.00001,"Sharpness supports precise keyboard adjustment");
        Call("SaveSettings");
        var picker=Get<ComboBox>("ProfileCombo");picker.SelectedIndex=0;slider.Value=0.65;Call("SaveSettings");
        picker.SelectedIndex=1;slider.Value=1.25;Call("SaveSettings");picker.SelectedIndex=0;
        Check(slider.Value==0.65,"Sharpness belongs to the selected profile");Call("SaveSettings");picker.SelectedIndex=1;
        Check(slider.Value==1.25,"Profiles retain separate sharpening strengths");Call("SaveSettings");
        var legacy=JsonNode.Parse(File.ReadAllText(library))!;
        foreach(var profile in legacy["Profiles"]!.AsArray()) profile!["Settings"]!.AsObject().Remove("Sharpness");
        File.WriteAllText(library,legacy.ToJsonString());Call("InitializeProfiles");Call("ApplyProfile",picker.SelectedItem);Call("SaveSettings");
        Check(slider.Value==0.3 && JsonNode.Parse(File.ReadAllText(library))!["Version"]!.GetValue<int>()==1,"Older profiles migrate sharpening to the native default without a schema reset");
        File.WriteAllText(ini,File.ReadAllText(ini).Replace("sharpness=0.30","sharpness=0.47").Replace("sharpen=1","sharpen=0"));
        Call("LoadState");Call("ApplyStateToUi");
        Check(slider.Value==0.47 && Get<CheckBox>("SharpenCheck").IsChecked==false,"Native menu strength and toggle take precedence when reloading");
        Get<CheckBox>("SharpenCheck").IsChecked=true;
        Render(Path.Combine(output,"sharpening-en.png"),1320,960);slider.BringIntoView();Pump();Render(Path.Combine(output,"sharpening-en.png"),1320,960);
        Select("UiLanguageCombo","vi");Render(Path.Combine(output,"sharpening-vi.png"),1320,960);
        Check(Get<TextBlock>("SharpnessTitle").Text=="Độ sắc nét" && Get<TextBlock>("SharpnessHint").Text.Contains("viền sáng"),"Sharpening controls and explanation translate to Vietnamese");
        Select("UiLanguageCombo","en");Call("SaveSettings");
    }

    static void VerifySettingsInteraction(string output,string root) {
        bool Dirty()=>(bool)typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!;
        void WaitForMotion() {
            var frame=new System.Windows.Threading.DispatcherFrame();
            var timer=new System.Windows.Threading.DispatcherTimer { Interval=TimeSpan.FromMilliseconds(350) };
            timer.Tick+=(_,_)=>{timer.Stop();frame.Continue=false;};timer.Start();System.Windows.Threading.Dispatcher.PushFrame(frame);
        }
        var viewport=Get<Grid>("ApplyBarViewport");var sharpness=Get<Slider>("SharpnessSlider");
        Get<CheckBox>("ReducedMotionCheck").IsChecked=true;Call("SaveSettings");Pump();
        Check(!Dirty() && viewport.Visibility==Visibility.Collapsed && viewport.Height==0,"Successful Apply collapses the whole bar and releases its layout space with reduced motion");
        foreach(string page in new[]{"Home","Display","Graphics","Controller","Mods","Online","Advanced","General"}) {
            ClickNav(page);window.UpdateLayout();Pump();
            var scroll=Get<ScrollViewer>(page=="Mods"?"ModsPage":page+"Page");
            scroll.ScrollToVerticalOffset(150);Pump();scroll.ScrollToTop();Pump();
            foreach(var bar in Visuals(scroll).OfType<System.Windows.Controls.Primitives.ScrollBar>()) {
                bar.RaiseEvent(new RoutedPropertyChangedEventArgs<double>(0,100,System.Windows.Controls.Primitives.RangeBase.ValueChangedEvent));
            }
            Check(!Dirty() && viewport.Visibility==Visibility.Collapsed,"Scrolling and scrollbar events do not stage edits in "+page);
        }
        ClickNav("Display");Render(Path.Combine(output,"apply-bar-hidden.png"),1000,680);
        var pageScroll=Get<ScrollViewer>("DisplayPage");pageScroll.ScrollToTop();Pump();
        var combo=Get<ComboBox>("ResolutionCombo");object selected=combo.SelectedItem;double offset=pageScroll.VerticalOffset;
        combo.Focus();
        var wheel=new MouseWheelEventArgs(Mouse.PrimaryDevice,Environment.TickCount,-120) {RoutedEvent=Mouse.PreviewMouseWheelEvent};
        combo.RaiseEvent(wheel);Pump();
        Check(wheel.Handled && ReferenceEquals(selected,combo.SelectedItem) && !Dirty(),"Wheel over a focused closed dropdown keeps its selection and clean state");
        Check(pageScroll.VerticalOffset>offset,"Wheel over a closed dropdown scrolls the settings page");
        combo.IsDropDownOpen=true;
        var openWheel=new MouseWheelEventArgs(Mouse.PrimaryDevice,Environment.TickCount,-120) {RoutedEvent=Mouse.PreviewMouseWheelEvent};
        combo.RaiseEvent(openWheel);
        Check(!openWheel.Handled,"Open dropdown lists retain their normal wheel behavior");combo.IsDropDownOpen=false;
        ClickNav("Graphics");Call("GraphicsTab_Click",Get<Button>("GraphicsUpscalingTab"),new RoutedEventArgs());
        sharpness.Value=0.55;Pump();
        Check(Dirty() && viewport.Visibility==Visibility.Visible && Get<Button>("ApplySettingsButton").IsEnabled,"Editing a real setting reveals Apply and its dirty indicator");
        Render(Path.Combine(output,"apply-bar-unsaved.png"),1320,960);
        Call("SaveSettings");Check(!Dirty() && viewport.Visibility==Visibility.Collapsed,"Apply hides the bar after a real setting edit");
        // A failed save must keep the pending edits and their retry action visible.
        bool online=Get<CheckBox>("OnlineEnabledCheck").IsChecked==true;string oldPort=Get<TextBox>("OnlineP2pPortBox").Text;
        Get<CheckBox>("OnlineEnabledCheck").IsChecked=true;Get<TextBox>("OnlineP2pPortBox").Text="65536";
        bool saved=(bool)typeof(MainWindow).GetMethod("SaveSettings",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        Check(!saved && Dirty() && viewport.Visibility==Visibility.Visible && Get<Button>("ApplySettingsButton").IsEnabled,"A failed Apply keeps its error and retry button visible");
        Get<CheckBox>("OnlineEnabledCheck").IsChecked=online;Get<TextBox>("OnlineP2pPortBox").Text=oldPort;Call("SaveSettings");
        Call("SetFooterMessage","Session ended · {0}",new object[]{23});
        Check(!Dirty() && viewport.Visibility==Visibility.Visible && !Get<Button>("ApplySettingsButton").IsEnabled,"Session notices can reveal the bar without creating unsaved changes");
        Call("SaveSettings");Get<CheckBox>("ReducedMotionCheck").IsChecked=false;WaitForMotion();
        Call("SaveSettings");
        if(SystemParameters.ClientAreaAnimation) Check(viewport.Visibility==Visibility.Visible && !viewport.IsHitTestVisible,"Normal Apply starts a noninteractive downward exit transition");
        WaitForMotion();
        Check(viewport.Visibility==Visibility.Collapsed && Math.Abs(viewport.Height)<0.001,"Apply completes its slide and collapses after the animation");
        sharpness.Value=0.65;WaitForMotion();Call("SaveSettings");sharpness.Value=0.75;WaitForMotion();
        Check(Dirty() && viewport.Visibility==Visibility.Visible && Math.Abs(viewport.Height-68)<0.001,"A new edit during the exit transition keeps Apply visible");
        Get<CheckBox>("ReducedMotionCheck").IsChecked=true;Call("SaveSettings");
    }

    static void VerifyUndo(string output,string root) {
        var undo=Get<Button>("UndoSettingsButton");
        bool Dirty()=>(bool)typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!;
        void Undo()=>undo.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        string ini=Path.Combine(root,"bbport.ini"), settings=Path.Combine(root,"launcher-settings.json"), library=Path.Combine(root,"user","launcher","profiles.json");
        string currentPath=Get<TextBox>("PathBox").Text;
        Select("UiLanguageCombo","en");Select("ThemeCombo","dark");Select("UpscalerCombo","dlss");Select("PresetCombo","1");Select("FpsCombo","60");
        Get<CheckBox>("ReducedMotionCheck").IsChecked=true;Get<Slider>("SharpnessSlider").Value=0.35;
        Get<CheckBox>("HdrOutputCheck").IsChecked=false;Get<CheckBox>("ShowCpuCheck").IsChecked=false;
        Get<CheckBox>("DeveloperModeCheck").IsChecked=false;Get<Slider>("MouseSensitivitySlider").Value=3;
        Call("SetBindingSlot",BindingRow("strong_attack"),"mouse","Left Shift + Mouse1");
        Get<CheckBox>("OnlineRememberCheck").IsChecked=false;Get<CheckBox>("OnlineEnabledCheck").IsChecked=false;
        Get<TextBox>("OnlineHostBox").Text="undo-baseline.example";Get<TextBox>("OnlineP2pPortBox").Text="32000";
        Get<PasswordBox>("OnlinePasswordBox").Password="undo-password-in-memory";Get<PasswordBox>("OnlineTokenBox").Password="undo-token-in-memory";
        Call("SaveSettings");
        string baselineIni=File.ReadAllText(ini),baselineState=File.ReadAllText(settings),baselineLibrary=File.ReadAllText(library);
        object baselineProfile=Get<ComboBox>("ProfileCombo").SelectedItem;
        Check(!Dirty() && !undo.IsEnabled,"Undo is disabled after successful Apply");
        Select("UpscalerCombo","taa");Select("FpsCombo","90");Get<Slider>("SharpnessSlider").Value=1.4;
        Get<Slider>("MouseSensitivitySlider").Value=8;Call("SetBindingSlot",BindingRow("strong_attack"),"mouse","Right Ctrl + Mouse2");
        Get<CheckBox>("HdrOutputCheck").IsChecked=true;Get<CheckBox>("ShowCpuCheck").IsChecked=true;Get<CheckBox>("DeveloperModeCheck").IsChecked=true;
        Get<TextBox>("OnlineHostBox").Text="unapplied.example";Get<PasswordBox>("OnlinePasswordBox").Password="changed";Get<PasswordBox>("OnlineTokenBox").Password="changed-token";
        Get<TextBox>("PathBox").Text=Path.Combine(root,"Not applied");Select("ThemeCombo","light");Select("UiLanguageCombo","vi");
        Check(Dirty() && undo.IsEnabled && undo.Content?.ToString()=="Hoàn tác" && undo.ToolTip?.ToString()?.Contains("toàn bộ")==true,"Undo is enabled for pending edits and translated with its scope");
        Render(Path.Combine(output,"undo-vi.png"),1320,920);
        Undo();Pump();
        Check(!Dirty() && !undo.IsEnabled && Get<Grid>("ApplyBarViewport").Visibility==Visibility.Collapsed,"Undo clears pending changes and collapses the Apply bar");
        Check(Tag("UpscalerCombo")=="dlss" && Tag("FpsCombo")=="60" && Get<Slider>("SharpnessSlider").Value==0.35,"Undo restores graphics and presentation together");
        Check(Get<Slider>("MouseSensitivitySlider").Value==3 && BindingRow("strong_attack").MouseToken=="Left Shift + Mouse1","Undo restores bindings and sensitivity, including the custom binding snapshot");
        Check(Get<CheckBox>("HdrOutputCheck").IsChecked==false && Get<CheckBox>("ShowCpuCheck").IsChecked==false && Get<CheckBox>("DeveloperModeCheck").IsChecked==false,"Undo also restores HDR, overlay and diagnostic switches");
        Check(Get<TextBox>("PathBox").Text==currentPath && Tag("ThemeCombo")=="dark" && Localization.Current.Language=="en","Undo restores installation path and previewed appearance/language");
        Check(Get<TextBox>("OnlineHostBox").Text=="undo-baseline.example" && Get<PasswordBox>("OnlinePasswordBox").Password=="undo-password-in-memory" && Get<PasswordBox>("OnlineTokenBox").Password=="undo-token-in-memory","Undo restores in-memory online fields without reading a remembered account");
        Check(File.ReadAllText(ini)==baselineIni && File.ReadAllText(settings)==baselineState && File.ReadAllText(library)==baselineLibrary,"Undo writes no runtime settings, launcher settings or profile library");
        Check(!baselineState.Contains("undo-password") && !baselineIni.Contains("undo-password") && !baselineLibrary.Contains("undo-password"),"Undo account checkpoints never enter settings/profile files");
        var picker=Get<ComboBox>("ProfileCombo");picker.SelectedIndex=ReferenceEquals(picker.Items[0],baselineProfile)?1:0;Pump();
        Check(Dirty() && !ReferenceEquals(picker.SelectedItem,baselineProfile),"Profile selection is staged before Undo");Undo();
        Check(ReferenceEquals(picker.SelectedItem,baselineProfile) && !Dirty() && File.ReadAllText(library)==baselineLibrary,"Undo restores the previously applied profile without changing profile membership");
        Get<Slider>("SharpnessSlider").Value=0.7;Call("SaveSettings");Get<Slider>("SharpnessSlider").Value=1.8;Undo();
        Check(Get<Slider>("SharpnessSlider").Value==0.7 && !Dirty(),"A successful Apply advances the Undo checkpoint");
        Get<TextBox>("OnlineP2pPortBox").Text="65536";Get<Slider>("SharpnessSlider").Value=1.6;
        bool saved=(bool)typeof(MainWindow).GetMethod("SaveSettings",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        string failedIni=File.ReadAllText(ini),failedSettings=File.ReadAllText(settings);
        Check(!saved && undo.IsEnabled,"Undo remains available after a failed Apply");Undo();
        Check(Get<Slider>("SharpnessSlider").Value==0.7 && Get<TextBox>("OnlineP2pPortBox").Text=="32000" && !Dirty(),"Failed Apply never replaces the last successful checkpoint");
        Check(File.ReadAllText(ini)==failedIni && File.ReadAllText(settings)==failedSettings,"Undo after a failed save still performs no file writes");
        Call("SaveSettings");Get<Slider>("SharpnessSlider").Value=0.8;
        Render(Path.Combine(output,"undo-en.png"),1320,920);
        window.MinWidth=640;window.MinHeight=480;Select("UiLanguageCombo","vi");Render(Path.Combine(output,"undo-compact-vi.png"),640,480);
        Check(FitsWithin(undo,Get<FrameworkElement>("ApplyBar")) && FitsWithin(Get<Button>("ApplySettingsButton"),Get<FrameworkElement>("ApplyBar")),"Undo and Apply fit together in the smallest Vietnamese layout");
        Undo();Call("SaveSettings");Call("SetFooterMessage","Session ended · {0}",new object[]{23});
        Check(!undo.IsEnabled && !Dirty(),"A session notice alone does not enable Undo");Call("SaveSettings");
    }

    static void VerifyMods(string output,string root) {
        string mods=Path.Combine(root,"mods"),config=Path.Combine(root,"mods.json"),game=Get<TextBox>("PathBox").Text;
        string python=new[]{Path.Combine(sourceRoot,"tools/python/python.exe"),Path.Combine(sourceRoot,"builds/_internal/toolchain/clang64/bin/python.exe"),Path.Combine(Environment.GetEnvironmentVariable("BB_MSYS2")??@"C:\msys64","clang64/bin/python.exe")}.FirstOrDefault(File.Exists)
            ?? throw new InvalidOperationException("Mod checks require packaged Python or CLANG64 Python from BB_MSYS2.");
        string RunPython(params string[] arguments) {
            var start=new System.Diagnostics.ProcessStartInfo(python) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
            foreach(string argument in arguments)start.ArgumentList.Add(argument);
            using var process=System.Diagnostics.Process.Start(start)!;
            var stdout=process.StandardOutput.ReadToEndAsync();var stderr=process.StandardError.ReadToEndAsync();
            if(!process.WaitForExit(15000)){process.Kill(true);throw new Exception("Synthetic mod command timed out");}
            if(process.ExitCode!=0)throw new Exception(stderr.GetAwaiter().GetResult());
            return stdout.GetAwaiter().GetResult().Trim();
        }
        void Put(string file,string text) {Directory.CreateDirectory(Path.GetDirectoryName(file)!);File.WriteAllText(file,text);}
        void Scan() {
            var type=typeof(MainWindow).Assembly.GetType("BloodborneLauncher.ModCatalog")!;
            object catalog=Task.Run(async()=>{
                var task=(Task)type.GetMethod("Scan",BindingFlags.Static|BindingFlags.NonPublic)!.Invoke(null,new object[]{sourceRoot,mods,game})!;
                await task.ConfigureAwait(false);return task.GetType().GetProperty("Result")!.GetValue(task)!;
            }).GetAwaiter().GetResult();
            Call("UpdateModCatalog",catalog);Pump();
        }
        var list=Get<ListBox>("ModList");
        MainWindow.ModRow Row(string name)=>list.Items.Cast<MainWindow.ModRow>().Single(row=>row.Name==name);
        bool Dirty()=>(bool)typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!;
        void Undo()=>Get<Button>("UndoSettingsButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        string shared="dvdroot_ps4/chr/a.dcx";
        Put(Path.Combine(game,shared),"base");Put(Path.Combine(mods,"Costume",shared),"costume");Put(Path.Combine(mods,"Lighting",shared),"lighting");
        Put(Path.Combine(mods,"Lighting/dvdroot_ps4/sfx/new.dcx"),"new");Put(Path.Combine(mods,"Download.zip"),"archive fixture");Put(Path.Combine(mods,"Unknown/readme.txt"),"not a recognized layout");
        File.WriteAllText(config,"{\"order\":[\"Costume\",\"Lighting\"],\"disabled\":[],\"future_note\":\"keep\"}");
        Call("LoadModSettings");Get<CheckBox>("ModsEnabledCheck").IsChecked=true;Call("SaveSettings");
        ClickNav("Mods");Call("SavesTab_Click",Get<Button>("ModsTab"),new RoutedEventArgs());Scan();
        Check(!Dirty() && list.Items.Count==4,"Catalog shows valid and unrecognized entries without staging edits");
        Check(Row("Costume").Enabled && Row("Lighting").Enabled && !Row("Download.zip").CanToggle && !Row("Unknown").CanToggle,"Recognized mods can be toggled; archives and bad layouts explain why they cannot load");
        Check(Get<ItemsControl>("ModConflictList").Items.Cast<MainWindow.ModConflict>().Single().Winner=="Lighting","Conflict preview matches the runtime's last-mod-wins order");
        string before=File.ReadAllText(config);Row("Lighting").Enabled=false;
        Check(Dirty() && File.ReadAllText(config)==before && Get<ItemsControl>("ModConflictList").Items.Count==0,"Individual toggles update preview but do not write before Apply");
        Undo();Check(Row("Lighting").Enabled && !Dirty() && File.ReadAllText(config)==before,"Undo restores pending mod choices without writing mods.json");
        list.SelectedItem=Row("Costume");Call("MoveMod_Click",Get<Button>("ModMoveDownButton"),new RoutedEventArgs());
        Check(Get<ItemsControl>("ModConflictList").Items.Cast<MainWindow.ModConflict>().Single().Winner=="Costume","Moving a mod changes the predicted winning file");
        Call("SaveSettings");
        var stored=JsonNode.Parse(File.ReadAllText(config))!;
        Check(stored["order"]!.AsArray().Select(value=>value!.GetValue<string>()).SequenceEqual(new[]{"Lighting","Costume"}) && stored["future_note"]!.GetValue<string>()=="keep","Apply writes runtime-compatible order and preserves unknown config fields");
        Check(File.Exists(config+".bak"),"Mod settings keep an atomic previous-file backup");
        string overlay=RunPython(Path.Combine(sourceRoot,"scripts/mods.py"),game,"--mods-dir",mods,"--config",config,"--out",Path.Combine(root,"mod-test-out"));
        Check(File.ReadAllText(Path.Combine(overlay,shared))=="costume" && File.ReadAllText(Path.Combine(game,shared))=="base","The actual Python loader uses the launcher's saved order and leaves the base game untouched");
        RunPython("-c","import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import mods; mods.remove_overlay(Path(sys.argv[2]))",Path.Combine(sourceRoot,"scripts"),overlay);
        Row("Costume").Enabled=false;Call("SaveSettings");Call("LoadModSettings");Scan();
        Check(!Row("Costume").Enabled && Row("Lighting").Enabled && !Dirty(),"Per-mod enable state survives Apply and reload");
        list.SelectedItem=Row("Lighting");Get<ScrollViewer>("ModsPage").ScrollToVerticalOffset(350);Pump();
        Check(!Dirty(),"Selecting or scrolling mod rows does not create unsaved changes");
        Get<CheckBox>("ModsEnabledCheck").IsChecked=false;
        Check(Get<ItemsControl>("ModConflictList").Items.Count==0 && Get<TextBlock>("ModSummary").Text.Contains("disabled"),"Master Off removes all effective mod conflicts");Undo();
        Put(Path.Combine(mods,"BadExecutable",shared),"bad");Put(Path.Combine(mods,"BadExecutable/eboot.bin"),"unsupported executable");Scan();
        Check(Row("BadExecutable").Enabled && Row("BadExecutable").Status.Contains("Cannot load"),"Unsafe executable replacement is shown as an invalid mod");
        bool saved=(bool)typeof(MainWindow).GetMethod("SaveSettings",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        Check(!saved,"An active invalid mod must be disabled before Apply");Row("BadExecutable").Enabled=false;Call("SaveSettings");
        Check(JsonNode.Parse(File.ReadAllText(config))!["disabled"]!.AsArray().Any(value=>value!.GetValue<string>()=="BadExecutable"),"Disabling the invalid mod permits other mods to load");
        Row("Costume").Enabled=true;Call("SaveSettings");
        Render(Path.Combine(output,"mods-manager-en.png"),1320,960);Get<ScrollViewer>("ModsPage").ScrollToTop();Render(Path.Combine(output,"mods-manager-en.png"),1320,960);
        Select("UiLanguageCombo","vi");Render(Path.Combine(output,"mods-manager-vi.png"),1320,960);
        Check(Row("Lighting").Status.Contains("file trùng") && Get<Button>("ModMoveUpButton").Content?.ToString()=="Đưa lên","Mod rows, priorities and controls translate to Vietnamese");
        window.MinWidth=640;window.MinHeight=480;Render(Path.Combine(output,"mods-manager-compact-vi.png"),800,680);
        Get<Button>("ModRefreshButton").BringIntoView();Pump();
        Check(FitsWithin(Get<Button>("ModRefreshButton"),Get<ScrollViewer>("ModsPage")),"Mod actions remain reachable in compact layouts");
        Select("UiLanguageCombo","en");Call("SaveSettings");
        string validConfig=File.ReadAllText(config);File.WriteAllText(config,"{broken");Call("LoadModSettings");
        Check(!list.IsEnabled && Get<TextBlock>("ModConfigWarning").Visibility==Visibility.Visible,"Malformed configuration is visible and cannot be silently overwritten");
        Get<CheckBox>("ModsEnabledCheck").IsChecked=false;Call("SaveSettings");
        Check(File.ReadAllText(config)=="{broken","Global Off leaves malformed mod configuration intact");
        File.WriteAllText(config,validConfig);Call("LoadModSettings");Scan();Get<CheckBox>("ModsEnabledCheck").IsChecked=true;Call("SaveSettings");
        Check(list.IsEnabled && !Dirty(),"A repaired mod configuration can be loaded without resetting its choices");
    }

    static void VerifyLighting(string output,string root) {
        var names=new[]{"GraphicsAoSlider","GraphicsShadowsSlider","GraphicsBloomSlider","GraphicsSaturationSlider"};
        string ini=Path.Combine(root,"bbport.ini"), state=Path.Combine(root,"launcher-settings.json"), profiles=Path.Combine(root,"user","launcher","profiles.json");
        bool RuntimeEnabled()=>File.ReadAllLines(ini).Contains("graphics_controls=1");
        bool ProfileEnabled() {
            var data=JsonNode.Parse(File.ReadAllText(profiles))!;
            return data["Profiles"]!.AsArray().Single(p=>p!["Id"]!.GetValue<string>()==data["ActiveProfileId"]!.GetValue<string>())!["Settings"]!["GraphicsControls"]!.GetValue<bool>();
        }
        void Reset()=>Call("ResetLighting_Click",Get<Button>("ResetLightingButton"),new RoutedEventArgs());
        ClickNav("Graphics");Call("GraphicsTab_Click",Get<Button>("GraphicsEffectsTab"),new RoutedEventArgs());
        Check(window.FindName("GraphicsControlsCheck")==null,"Lighting controls have no master checkbox");
        string upscaler=Tag("UpscalerCombo"), fov=Tag("CameraFovCombo");
        bool? ssao=Get<CheckBox>("SsaoCheck").IsChecked, aa=Get<CheckBox>("GameAaCheck").IsChecked, hdr=Get<CheckBox>("HdrOutputCheck").IsChecked;
        string before=File.ReadAllText(ini), profileBefore=File.ReadAllText(profiles);
        Reset();
        Check(names.All(n=>Tag(n)=="1.00") && Get<CheckBox>("GraphicsVignetteCheck").IsChecked==true,"Restore defaults resets lighting to original multipliers and vignette On");
        Check(File.ReadAllText(ini)==before && File.ReadAllText(profiles)==profileBefore,"Restore defaults stages the change until Apply or Play");
        Check(Tag("UpscalerCombo")==upscaler && Tag("CameraFovCombo")==fov && Get<CheckBox>("SsaoCheck").IsChecked==ssao && Get<CheckBox>("GameAaCheck").IsChecked==aa && Get<CheckBox>("HdrOutputCheck").IsChecked==hdr,"Lighting reset preserves AA, SSAO, camera, upscaling and HDR settings");
        Call("SaveSettings");
        Check(!RuntimeEnabled() && !ProfileEnabled(),"Original lighting does not require runtime hooks");
        var saturation=Get<Slider>("GraphicsSaturationSlider");
        string savedBeforeDrag=File.ReadAllText(ini);
        saturation.Value=137;
        Check(Get<TextBlock>("GraphicsSaturationValue").Text=="137%" && File.ReadAllText(ini)==savedBeforeDrag,"Slider readouts update immediately without writing settings while dragging");
        Slider.IncreaseSmall.Execute(null,saturation);
        Check(saturation.Value==138 && Get<TextBlock>("GraphicsSaturationValue").Text=="138%","Keyboard adjustment changes the effect by one percent");
        Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
        Check(saturation.Value==138 && File.ReadAllLines(ini).Contains("graphics_saturation=1.38"),"Slider percentages persist as runtime multipliers with one percent precision");
        foreach(var (name,value) in new[]{("GraphicsAoSlider","0.40"),("GraphicsShadowsSlider","1.40"),("GraphicsBloomSlider","0.60"),("GraphicsSaturationSlider","1.20"),("GraphicsAoSlider","0.00"),("GraphicsBloomSlider","0.00"),("GraphicsSaturationSlider","0.00")}) {
            Reset();Select(name,value);Call("SaveSettings");Call("LoadState");Call("ApplyStateToUi");
            Check(Tag(name)==value && names.Where(n=>n!=name).All(n=>Tag(n)=="1.00") && Get<CheckBox>("GraphicsVignetteCheck").IsChecked==true,"Independent lighting override survives Apply/reload: "+name+"="+value);
            Check(RuntimeEnabled() && ProfileEnabled() && JsonNode.Parse(File.ReadAllText(state))!["GraphicsControls"]!.GetValue<bool>(),"Runtime and profile support is derived automatically for "+name+"="+value);
        }
        Reset();Get<CheckBox>("GraphicsVignetteCheck").IsChecked=false;Call("SaveSettings");
        Check(RuntimeEnabled() && ProfileEnabled() && names.All(n=>Tag(n)=="1.00"),"Vignette alone enables support without changing numeric effects");
        Reset();Call("SaveSettings");
        Check(!RuntimeEnabled() && !ProfileEnabled(),"Restoring all originals removes the internal override flag");
        // Existing v1 profiles can contain either value of the retired master switch.
        foreach(bool legacyFlag in new[]{false,true}) {
            var data=JsonNode.Parse(File.ReadAllText(profiles))!;
            var active=data["Profiles"]!.AsArray().Single(p=>p!["Id"]!.GetValue<string>()==data["ActiveProfileId"]!.GetValue<string>())!["Settings"]!;
            active["GraphicsControls"]=legacyFlag;active["GraphicsBloom"]=legacyFlag?"1.00":"0.40";
            File.WriteAllText(profiles,data.ToJsonString());Call("InitializeProfiles");
            Call("ApplyProfile",Get<ComboBox>("ProfileCombo").SelectedItem);Call("SaveSettings");
            Check(Tag("GraphicsBloomSlider")== (legacyFlag?"1.00":"0.40") && RuntimeEnabled()==!legacyFlag && ProfileEnabled()==!legacyFlag,"Legacy profile values remain effective independently of its old master flag "+legacyFlag);
        }
        // The native INI remains authoritative for values, but the old switch no longer hides them.
        string original=File.ReadAllText(ini);
        File.WriteAllText(ini,original.Replace("graphics_bloom=1.00","graphics_bloom=0.37").Replace("graphics_controls=1","graphics_controls=0"));
        Call("LoadState");Call("ApplyStateToUi");Call("SaveSettings");
        Check(Tag("GraphicsBloomSlider")=="0.37" && RuntimeEnabled(),"Intermediate in-game values survive migration and enable their required hooks");
        Check(Get<Slider>("GraphicsBloomSlider").Value==37 && Get<TextBlock>("GraphicsBloomValue").Text=="37%","In-game multiplier values map to slider percentages correctly");
        Reset();Call("SaveSettings");
        Get<ScrollViewer>("GraphicsPage").ScrollToTop();Render(Path.Combine(output,"lighting-original-en.png"),1320,960);
        Check(new[]{"Ao","Shadows","Bloom","Saturation"}.All(n=>Get<TextBlock>("Graphics"+n+"Value").Text=="100% — Original"),"Original percentages are explicit in English");
        Select("UiLanguageCombo","vi");Pump();Render(Path.Combine(output,"lighting-original-vi.png"),1320,960);
        Check(new[]{"Ao","Shadows","Bloom","Saturation"}.All(n=>Get<TextBlock>("Graphics"+n+"Value").Text=="100% — Gốc"),"Original percentages translate without changing values");
        Check(Get<TextBlock>("LightingTitle").Text=="Ánh sáng và màu sắc" && Get<TextBlock>("LightingRestartHint").Text.Contains("khởi động lại game"),"Lighting title and restart guidance are localized");
        window.MinWidth=640;window.MinHeight=480;
        Render(Path.Combine(output,"lighting-compact-vi.png"),800,680);
        Get<Button>("ResetLightingButton").BringIntoView();Pump();
        Check(FitsWithin(Get<Button>("ResetLightingButton"),Get<ScrollViewer>("GraphicsPage")),"Reset lighting remains reachable in the compact layout");
        Render(Path.Combine(output,"lighting-compact-vi.png"),800,680);
        Select("UiLanguageCombo","en");Call("SaveSettings");
    }

    static void VerifyProfiles(string output,string root) {
        window.MinWidth=1000; window.MinHeight=680;
        Select("UiLanguageCombo","en"); Select("ThemeCombo","dark");
        Select("UpscalerCombo","dlss"); Select("PresetCombo","1"); Select("PresentCombo","Immediate");
        Get<ComboBox>("ResolutionCombo").SelectedItem=Get<ComboBox>("ResolutionCombo").Items.Cast<ComboBoxItem>().Single(item=>item.Content?.ToString()=="1920x1080"); Select("FpsCombo","60");
        BindingRow("strong_attack").Binding="Left Shift + Mouse1";
        Get<Slider>("MouseSensitivitySlider").Value=8;
        Get<TextBox>("OnlineHostBox").Text="private-server.example";
        Get<PasswordBox>("OnlinePasswordBox").Password="memory-only-profile-test";
        Get<PasswordBox>("OnlineTokenBox").Password="token-only-profile-test";
        Call("SaveSettings");
        string path=Path.Combine(root,"user","launcher","profiles.json");
        Check(File.Exists(path),"Existing settings migrate to a local profile library");
        var initial=JsonNode.Parse(File.ReadAllText(path))!;
        Check(initial["Version"]!.GetValue<int>()==1 && initial["Profiles"]!.AsArray().Count==1,"Versioned library begins with current setup");
        string originalId=initial["ActiveProfileId"]!.GetValue<string>();
        var initialSettings=initial["Profiles"]![0]!["Settings"]!.AsObject();
        Check(!initialSettings.ContainsKey("GamePath") && !initialSettings.ContainsKey("UiLanguage") && !initialSettings.ContainsKey("UiTheme"),"Profiles exclude paths and launcher preferences");
        Check(!initialSettings.Any(p=>p.Key.StartsWith("Online")) && !File.ReadAllText(path).Contains("private-server.example") && !File.ReadAllText(path).Contains("memory-only"),"Profiles exclude online addresses and credentials");
        Check(initial["Profiles"]![0]!["Bindings"]!["strong_attack"]!.GetValue<string>()=="Left Shift + Mouse1","Profiles preserve keyboard and mouse combinations");
        var create=typeof(MainWindow).GetMethod("CreateProfileCopy",BindingFlags.Instance|BindingFlags.NonPublic)!;
        Check((bool)create.Invoke(window,new object[]{"Boss fights"})!,"Save a copy creates and applies a named profile");
        Check(Get<ComboBox>("ProfileCombo").Items.Count==2 && Get<ComboBox>("ProfileCombo").Text=="Boss fights","New profile appears in the global selector");
        Check(!((bool)typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!),"Saving clears the dirty state");
        Select("UpscalerCombo","taa"); Select("PresentCombo","FifoRelaxed"); Select("FpsCombo","90");
        Get<Slider>("MouseSensitivitySlider").Value=2;
        BindingRow("strong_attack").Binding="Right Ctrl + Mouse2";
        Call("SaveSettings");
        var profiles=Get<ComboBox>("ProfileCombo");
        string idBefore=JsonNode.Parse(File.ReadAllText(path))!["ActiveProfileId"]!.GetValue<string>();
        string iniBefore=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        profiles.SelectedIndex=0; Pump();
        Check(Tag("UpscalerCombo")=="dlss" && Tag("PresentCombo")=="Immediate" && Tag("FpsCombo")=="60","Changing profile restores graphics, presentation and FPS independently");
        Check(BindingRow("strong_attack").Binding=="Left Shift + Mouse1" && Get<Slider>("MouseSensitivitySlider").Value==8,"Changing profile restores input bindings and sensitivity");
        Check(Get<PasswordBox>("OnlinePasswordBox").Password=="memory-only-profile-test" && Get<PasswordBox>("OnlineTokenBox").Password=="token-only-profile-test","Switching profiles preserves unsaved in-memory online credentials");
        Check(File.ReadAllText(Path.Combine(root,"bbport.ini"))==iniBefore,"Selecting a profile stages settings without writing runtime configuration");
        Check(JsonNode.Parse(File.ReadAllText(path))!["ActiveProfileId"]!.GetValue<string>()==idBefore,"The active profile is persisted when applied");
        Check(Get<System.Windows.Shapes.Ellipse>("ProfileDirtyDot").Visibility==Visibility.Visible,"Staged profile visibly has unapplied changes");
        Call("SaveSettings");
        Check(JsonNode.Parse(File.ReadAllText(path))!["ActiveProfileId"]!.GetValue<string>()==originalId,"Apply persists the selected profile identity");
        profiles.SelectedIndex=1; Pump();
        Check(Tag("UpscalerCombo")=="taa" && Tag("PresentCombo")=="FifoRelaxed" && !Get<ComboBox>("PresetCombo").IsEnabled,"TAA and present mode remain correct after a profile round trip");
        Check(BindingRow("strong_attack").Binding=="Right Ctrl + Mouse2" && Get<Slider>("MouseSensitivitySlider").Value==2,"Each named profile keeps its own inputs");
        Call("SaveSettings"); Call("InitializeProfiles");
        Check(profiles.Text=="Boss fights" && profiles.Items.Count==2,"Profile names and active selection survive reloading");
        Check(File.Exists(path+".bak"),"Atomic saves retain a previous library backup");
        int count=profiles.Items.Count;
        try { create.Invoke(window,new object[]{"boss FIGHTS"}); throw new Exception("Duplicate profile was accepted"); }
        catch(TargetInvocationException error) when(error.InnerException is InvalidDataException) { assertions++; }
        Check(profiles.Items.Count==count,"A duplicate name cannot overwrite another profile");
        string good=File.ReadAllText(path);
        File.WriteAllText(path,"{\"Version\":1,\"Profiles\":[");
        Call("InitializeProfiles");
        Check(profiles.Items.Count==2 && Directory.EnumerateFiles(Path.GetDirectoryName(path)!,"profiles.json.corrupt-*").Any(),"A damaged library is recovered from backup and preserved for inspection");
        Call("SaveSettings");
        Check(JsonNode.Parse(File.ReadAllText(path))!["Profiles"]!.AsArray().Count==2,"Recovered profiles can be saved normally");
        // Exercise failed replacements and future formats using a separate store, never the live library.
        var storeType=typeof(MainWindow).Assembly.GetType("BloodborneLauncher.LauncherProfileStore")!;
        string isolated=Path.Combine(root,"profile-store-tests","profiles.json");
        Directory.CreateDirectory(Path.GetDirectoryName(isolated)!); File.WriteAllText(isolated,good);
        object store=Activator.CreateInstance(storeType,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic,null,new object[]{isolated},null)!;
        var load=storeType.GetMethod("Load",BindingFlags.Instance|BindingFlags.NonPublic)!;
        var save=storeType.GetMethod("Save",BindingFlags.Instance|BindingFlags.NonPublic)!;
        object library=load.Invoke(store,new object?[]{null})!;
        using(var locked=new FileStream(isolated,FileMode.Open,FileAccess.Read,FileShare.Read)) {
            try { save.Invoke(store,new[]{library}); throw new Exception("A locked profile library was replaced"); }
            catch(TargetInvocationException error) when(error.InnerException is IOException) { assertions++; }
        }
        Check(File.ReadAllText(isolated)==good && !Directory.EnumerateFiles(Path.GetDirectoryName(isolated)!,"*.tmp").Any(),"A failed atomic save leaves the original bytes and cleans its temporary file");
        var future=JsonNode.Parse(good)!; future["Version"]=99; File.WriteAllText(isolated,future.ToJsonString());
        try { load.Invoke(store,new object?[]{null}); throw new Exception("A future profile format was accepted"); }
        catch(TargetInvocationException error) when(error.InnerException is NotSupportedException) { assertions++; }
        Check(JsonNode.Parse(File.ReadAllText(isolated))!["Version"]!.GetValue<int>()==99,"Unsupported profile versions are preserved without downgrade");
        // Inspect the new pages with real, independent controls and both themes.
        ClickNav("Graphics"); Get<ScrollViewer>("GraphicsPage").ScrollToTop();
        Select("UpscalerCombo","dlss"); Select("PresetCombo","2");
        Check(Get<TextBlock>("SceneResolutionText").Text=="1130 × 636","Render preview uses the runtime's even-dimension rounding for Balanced");
        Select("UpscalerCombo","off");
        Check(Get<TextBlock>("SceneResolutionText").Text=="1920 × 1080" && !Get<ComboBox>("PresetCombo").IsEnabled,"Off shows native scene size and disables quality");
        Select("UpscalerCombo","dlss"); Select("PresetCombo","1");
        Render(Path.Combine(output,"control-hub-graphics.png"),1320,920);
        Call("GraphicsTab_Click",Get<Button>("GraphicsEffectsTab"),new RoutedEventArgs());
        Check(Get<StackPanel>("EffectsPanel").Visibility==Visibility.Visible,"Effects tab displays the existing effect controls");
        Render(Path.Combine(output,"control-hub-effects.png"),1320,920);
        Call("GraphicsTab_Click",Get<Button>("GraphicsCameraTab"),new RoutedEventArgs());
        Check(Get<ComboBox>("CameraFovCombo").IsVisible,"Camera settings have their own graphics tab");
        Call("GraphicsTab_Click",Get<Button>("GraphicsUpscalingTab"),new RoutedEventArgs());
        ClickNav("Controller"); Get<ScrollViewer>("ControllerPage").ScrollToTop();
        Render(Path.Combine(output,"control-hub-controls.png"),1320,920);
        Get<Button>("KeyForward").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(Get<Grid>("CaptureOverlay").Visibility==Visibility.Visible,"Movement diagram starts the same key capture as the binding table");
        Call("CancelCapture_Click",window,new RoutedEventArgs());
        Call("ControlsTab_Click",Get<Button>("GamepadTab"),new RoutedEventArgs());
        window.UpdateLayout(); Pump();
        Check(Get<ComboBox>("ControllerCombo").IsVisible && !Get<DataGrid>("KeyBindingsGrid").IsVisible,"Gamepad tab separates controller selection from keyboard bindings");
        Call("ControlsTab_Click",Get<Button>("KeyboardTab"),new RoutedEventArgs());
        ClickNav("General"); Select("ThemeCombo","light"); Render(Path.Combine(output,"control-hub-settings-light.png"),1320,920);
        Check(((SolidColorBrush)Application.Current.Resources["PanelBrush"]).Color==Color.FromRgb(255,255,255),"Light theme updates native product surfaces");
        Select("UiLanguageCombo","vi"); ClickNav("Display");
        Render(Path.Combine(output,"control-hub-display-light-vi.png"),1320,920);
        Select("ThemeCombo","dark"); Select("UiLanguageCombo","en"); ClickNav("Home"); Get<ScrollViewer>("HomePage").ScrollToTop();
        Render(Path.Combine(output,"control-hub-play.png"),1320,920);
        Call("SaveSettings");
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
        string? verificationMsys2=Environment.GetEnvironmentVariable("BB_MSYS2");
        foreach (string key in Environment.GetEnvironmentVariables().Keys)
            if(key.StartsWith("BB_") && key != "BB_MSYS2") Environment.SetEnvironmentVariable(key,null);
        Check(Environment.GetEnvironmentVariable("BB_MSYS2")==verificationMsys2,"Fixture cleanup preserves the configured MSYS2 Python location");
        if (File.Exists(Path.Combine(root,"launcher-settings.json"))) File.Delete(Path.Combine(root,"launcher-settings.json"));
        foreach(var stale in new[]{"out/game_dir.txt","launch-env.txt","launch-args.txt"}) { var f=Path.Combine(root,stale); if(File.Exists(f)) File.Delete(f); }
        var app = new App();
        app.InitializeComponent();
        System.Globalization.CultureInfo.CurrentUICulture=new System.Globalization.CultureInfo("vi-VN");

        window = new MainWindow { ShowInTaskbar=false, ShowActivated=false, WindowStartupLocation=WindowStartupLocation.Manual, Left=-20000, Top=-20000 };
        window.Show(); Pump();
        Check(Get<Border>("HomeSessionCard").Visibility==(publicRelease?Visibility.Collapsed:Visibility.Visible),"The Play session summary follows the build profile before a game is launched");
        Check(!Get<Button>("UndoSettingsButton").IsEnabled,"Undo starts disabled with no pending edits");
        Get<Slider>("SharpnessSlider").Value=0.6;Get<Button>("UndoSettingsButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(Get<Slider>("SharpnessSlider").Value==0.3 && !Get<Button>("UndoSettingsButton").IsEnabled,"Undo before the first Apply restores startup settings");
        Get<CheckBox>("ReducedMotionCheck").IsChecked=true;
        Check(new[]{"GraphicsAoSlider","GraphicsShadowsSlider","GraphicsBloomSlider","GraphicsSaturationSlider"}.All(n=>Tag(n)=="1.00") && Get<CheckBox>("GraphicsVignetteCheck").IsChecked==true,"First launch uses original lighting and color settings");
        Check(Get<CheckBox>("ShowCpuCheck").IsChecked==false && Get<CheckBox>("ShowGpuCheck").IsChecked==false,"CPU/GPU monitoring is opt-in by default");
        Check(Get<Slider>("SharpnessSlider").Value==0.3,"First launch retains the native sharpening strength default");
        Check(window.Title=="Techmo Bloodborne · experiment-v2","Techmo experiment-v2 window branding");
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
        Check(FitsWithin(Get<ComboBox>("ProfileCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Profile selector reachable outside Home");
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
        Check(window.FindName("QuickSettingsGrid")==null && window.FindName("QuickFps")==null,"Play does not duplicate Display or Graphics settings");
        VerifyUpscalingAvailability(output,root);
        Get<CheckBox>("FullscreenCheck").IsChecked=true;
        Select("CameraFovCombo","1.25"); Select("CameraDistanceCombo","0.80"); Select("CameraHeightCombo","1.20");
        Select("GraphicsAoSlider","0.40"); Select("GraphicsShadowsSlider","1.40");
        Select("GraphicsBloomSlider","0.80"); Select("GraphicsSaturationSlider","1.20");
        Get<CheckBox>("RebirthCheck").IsChecked=true;
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
        foreach(var translatedPage in new[]{"Graphics","Display","Mods","Advanced"}) {
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
        CheckSessionCard("After game exit");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x0000008B\",\"outcome\":{\"category\":\"crash\",\"reason\":\"access_violation\"},\"performance\":{\"available\":true,\"average_fps\":59.5,\"one_percent_low_fps\":44.1,\"p99_frame_ms\":22.68}}");
        Call("ShowSessionReport",sessionDir,139);Pump();
        CheckSessionCard("Crash report");
        Get<Border>("LastSessionCard").Visibility=Visibility.Collapsed;
        Call("LoadLatestSession");Pump();
        CheckSessionCard("Restore previous session on startup");
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne stopped unexpectedly","Native crash is not reported as a normal close");
        Check(Get<TextBlock>("LastSessionDetail").Text.Contains("Memory access violation") && Get<TextBlock>("LastSessionDetail").Text.Contains("0x0000008B"),"Crash shows evidence and exit code");
        Check(Get<TextBlock>("LastSessionPerformance").Text.Contains("59.5") && Get<TextBlock>("LastSessionPerformance").Text.Contains("44.1"),"Measured FPS and 1% low displayed");
        Select("UiLanguageCombo","vi");Pump();
        CheckSessionCard("Vietnamese session report");
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne dừng bất thường" && Get<TextBlock>("LastSessionDetail").Text.Contains("Lỗi truy cập bộ nhớ"),"Existing crash result translates to Vietnamese");
        ClickNav("Home");Render(Path.Combine(output,"session-crash-vi.png"));
        Select("UiLanguageCombo","en");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x00000017\",\"outcome\":{\"category\":\"crash\",\"reason\":\"renderer_assertion\"}}");
        Call("ShowSessionReport",sessionDir,23);Pump();
        Check(Get<TextBlock>("LastSessionDetail").Text.Contains("internal assertion") && Get<TextBlock>("LastSessionDetail").Text.Contains("0x00000017"),"Renderer assertion is distinguished from a generic process exit");
        Select("UiLanguageCombo","vi");Pump();
        Check(Get<TextBlock>("LastSessionDetail").Text.Contains("kiểm tra nội bộ thất bại"),"Renderer assertion detail translates live");
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
        CheckSessionCard("Truncated report");
        File.WriteAllText(Path.Combine(sessionDir,"session.json"),"{\"status\":\"finished\",\"exit_code_hex\":\"0x00000000\",\"outcome\":{\"category\":\"normal\",\"reason\":\"normal_exit\"}}");
        Call("ShowSessionReport",sessionDir,0);Pump();
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne closed","A successful runtime exit retains the normal close message");
        CheckSessionCard("Normal exit report");
        File.WriteAllText(Path.Combine(root,"run.bat"),"@echo off\r\nset BB_ > launch-env.txt\r\n> \"%BB_SESSION_DIR%\\session.json\" echo {\"status\":\"finished\",\"exit_code_hex\":\"0x0000008B\",\"outcome\":{\"category\":\"crash\",\"reason\":\"access_violation\"}}\r\nping 127.0.0.1 -n 2 > nul\r\nexit /b 139\r\n");
        Get<CheckBox>("DeveloperModeCheck").IsChecked=false;
        Environment.SetEnvironmentVariable("BB_GPU_PROFILE","1");
        Get<CheckBox>("GpuProfileCheck").IsChecked=false;
        Call("Play_Click", Get<Button>("PlayButton"),new RoutedEventArgs());
        for(int i=0;i<120 && !window.IsVisible;i++) { Thread.Sleep(50); Pump(); }
        Check(window.IsVisible && Get<Button>("PlayButton").IsEnabled,"Launcher recovers after a real synthetic nonzero process exit");
        Check(Get<TextBlock>("LastSessionStatus").Text=="Bloodborne stopped unexpectedly" && Get<TextBlock>("LastSessionDetail").Text.Contains("Memory access violation"),"Exited handler reads the native outcome and shell exit code");
        CheckSessionCard("Native process exit handler");
        string diagnosticEnv=File.ReadAllText(envPath);
        Check(diagnosticEnv.Split("\r\n").Contains("BB_FRAME_STATS=1"),"Frame recording works without Developer Mode");
        Check(!diagnosticEnv.Split("\r\n").Contains("BB_GPU_PROFILE=1"),"Disabled GPU profiling overrides an inherited profiler switch");
        Environment.SetEnvironmentVariable("BB_GPU_PROFILE",null);
        Check(!diagnosticEnv.Contains("BB_SESSION_DIR="+sessionDir+"\r\n"),"Consecutive sessions have different report directories");
        Call("ShowSessionReport",sessionDir,0);Get<ScrollViewer>("HomePage").ScrollToTop();
        Render(Path.Combine(output,"launcher-play.png"));
        foreach(var page in new[]{"General","Graphics","Display","Controller","Online","Mods","Advanced"}) {
            ClickNav(page); Pump();
            Check(Get<FrameworkElement>(page+"Page").Visibility==Visibility.Visible,"Navigation "+page);
            Check(Get<FrameworkElement>("HomePage").Visibility==Visibility.Collapsed,"Home hidden "+page);
            Render(Path.Combine(output,"launcher-"+page.ToLowerInvariant()+".png"));
            Check(FitsWithin(Get<ComboBox>("ProfileCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Global profile selector on "+page);
        }
        ClickNav("Graphics");
        var upscaler = Get<ComboBox>("PresetCombo");
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
        if (!publicRelease) Check(Get<ScrollViewer>("HomePage").VerticalOffset>0,"Compact home scrolls to Diagnostics");
        CheckDiagnosticsAccess("Compact home");
        ClickNav("Advanced"); Get<Expander>("SystemDetails").IsExpanded=true; Pump();
        Get<ScrollViewer>("AdvancedPage").ScrollToBottom(); Pump();
        Check(FitsWithin(Get<TextBlock>("Fsr4Status"),Get<ScrollViewer>("AdvancedPage")),"System capability details remain reachable in Diagnostics");
        Render(Path.Combine(output,"launcher-system-details-compact.png"),1000,680);
        Get<Expander>("SystemDetails").IsExpanded=false;
        Get<ScrollViewer>("HomePage").ScrollToTop(); Pump();
        ClickNav("Graphics"); Render(Path.Combine(output,"launcher-graphics-compact.png"),1000,680);
        ClickNav("Home"); Render(Path.Combine(output,"launcher-150-percent.png"),1100,720,144);
        window.MinWidth=900; window.MinHeight=520;
        Render(Path.Combine(output,"launcher-200-percent.png"),900,520,192);
        Check(FitsWithin(Get<ComboBox>("ProfileCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Profile selector fits the small high-DPI layout");
        Get<Button>("PlayButton").BringIntoView(); Pump();
        Check(FitsWithin(Get<Button>("PlayButton"),Get<ScrollViewer>("HomePage")),"High-DPI home can reach Play");
        Render(Path.Combine(output,"launcher-play-200-percent.png"),900,520,192);
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        CheckDiagnosticsAccess("High-DPI home");
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
        Check(FitsWithin(Get<ComboBox>("ProfileCombo"),Get<FrameworkElement>("WorkspaceHeader")),"Vietnamese profile selector fits the minimum work area");
        Get<Button>("PlayButton").BringIntoView(); Pump();
        Check(FitsWithin(Get<Button>("PlayButton"),Get<ScrollViewer>("HomePage")),"Play remains reachable in the minimum work area");
        Render(Path.Combine(output,"launcher-small-play-vi.png"),640,480);
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        CheckDiagnosticsAccess("Minimum work area");
        Render(Path.Combine(output,"launcher-small-quick-vi.png"),640,480);
        Select("UiLanguageCombo","en");
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("UpscalerCombo")=="fsr411" && Tag("FpsCombo")=="90" && Tag("PresetCombo")=="3","Settings reload");
        Check(Get<ComboBox>("FpsCombo").Items.Count==6,"30/60/90/120/144/Unlimited presets from the new engine");
        Check(window.FindName("VSyncCheck")==null,"Present mode is the single presentation setting without a duplicate VSync checkbox");
        foreach(string fps in new[]{"30","60","90","uncap"}) {
            Select("FpsCombo",fps);
            foreach(string mode in new[]{"Immediate","FifoRelaxed","","Fifo"}) {
                Select("PresentCombo",mode);
                Call("SaveSettings"); Call("LoadState"); Call("ApplyStateToUi");
                Check(Tag("FpsCombo")==fps && Tag("PresentCombo")==mode,"Independent FPS/present-mode round trip: "+fps+" / "+mode);
                Check(Get<TextBlock>("PresentModeHint").Text.Length>20,"Present mode has a useful explanation");
            }
            if(fps=="uncap") {
                ClickNav("Display");
                Render(Path.Combine(output,"display-unlimited-fifo-en.png"),1320,920);
                Select("UiLanguageCombo","vi");
                Render(Path.Combine(output,"display-unlimited-fifo-vi.png"),1320,920);
                Select("UiLanguageCombo","en");
            }
        }
        Select("FpsCombo","90"); Select("PresentCombo","FifoRelaxed"); Call("SaveSettings");
        foreach(string legacy in new[]{"Unlimited","0","480","bad"}) {
            string saved=File.ReadAllText(Path.Combine(root,"launcher-settings.json"));
            File.WriteAllText(Path.Combine(root,"launcher-settings.json"),saved.Replace("\"Fps\": \"90\"","\"Fps\": \""+legacy+"\""));
            Call("LoadState"); Call("ApplyStateToUi");
            Check(Tag("FpsCombo")=="60","Legacy FPS fallback "+legacy);
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
        Check(Get<CheckBox>("RebirthCheck").IsChecked==true,"Enhancements reload from INI");
        Check(Tag("GraphicsAoSlider")=="0.40" && Tag("GraphicsShadowsSlider")=="1.40" && Tag("GraphicsBloomSlider")=="0.80" && Tag("GraphicsSaturationSlider")=="1.20","Live graphics values reload");
        string runtimeIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),runtimeIni.Replace("graphics_bloom=0.80","graphics_bloom=0.60").Replace("rebirth=1","rebirth=0").Replace("effect_ssao=0","effect_ssao=1"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("GraphicsBloomSlider")=="0.60" && Get<CheckBox>("RebirthCheck").IsChecked==false && Get<CheckBox>("SsaoCheck").IsChecked==true,"Native pages remain authoritative over stale launcher JSON");
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
        VerifyProfiles(output,root);
        VerifySavesAndHdr(output,root);
        VerifyLighting(output,root);
        VerifyOverlaySettings(output,root);
        VerifySharpening(output,root);
        VerifySettingsInteraction(output,root);
        VerifyUndo(output,root);
        VerifyMods(output,root);
        Console.WriteLine($"PASS: {assertions} assertions; profiles, settings, navigation, input capture, launch environment and artwork.");
        typeof(MainWindow).GetField("hasUnsavedChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(window,false);
        window.Close(); app.Shutdown();
    }
}




'@
$program = $program.Replace('__PUBLIC_RELEASE__', $PublicRelease.IsPresent.ToString().ToLowerInvariant())
Set-Content -Encoding utf8 -LiteralPath (Join-Path $verificationRoot 'Program.cs') -Value $program
$arguments = @('run','--project',(Join-Path $verificationRoot 'LauncherChecks.csproj'),'--configuration','Release','--property:NuGetAudit=false','--property:RestoreIgnoreFailedSources=true')
$arguments += "--property:TechmoPublicRelease=$($PublicRelease.IsPresent.ToString().ToLowerInvariant())"
if ($PackageCache) { $arguments += "--property:RestorePackagesPath=$([IO.Path]::GetFullPath($PackageCache))" }
$previousVerificationSource = $env:TECHMO_VERIFY_SOURCE_ROOT
$env:TECHMO_VERIFY_SOURCE_ROOT = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
try {
    if ($SdlDll) { & dotnet @arguments -- $PreviewDirectory ([IO.Path]::GetFullPath($SdlDll)) }
    else { & dotnet @arguments -- $PreviewDirectory }
} finally { $env:TECHMO_VERIFY_SOURCE_ROOT = $previousVerificationSource }
if ($LASTEXITCODE -ne 0) { throw 'Launcher verification failed.' }
Write-Host "Preview images: $PreviewDirectory"
Write-Host "Isolated test fixtures: $verificationRoot"
