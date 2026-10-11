// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;

namespace BloodborneLauncher;

public partial class MainWindow
{
    KeyBindingRow? captureRow;
    string captureSlot="keyboard";
    IInputElement? captureFocus;
    bool keypadEnter;
    readonly HashSet<Key> captureModifiers=new();

    void InitializeInputCapture()
    {
        PreviewKeyDown += InputCapture_KeyDown;
        PreviewKeyUp += InputCapture_KeyUp;
        PreviewMouseDown += InputCapture_MouseDown;
        PreviewMouseWheel += InputCapture_Wheel;
        Deactivated += InputCapture_Deactivated;
        Closed += (_,_) => EndInputCapture(restoreFocus:false);
        SourceInitialized += (_,_) => (PresentationSource.FromVisual(this) as HwndSource)?.AddHook(InputCapture_WindowMessage);
        MouseSensitivitySlider.ValueChanged += (_,_) => {if(!syncing)L10n.SetText(FooterMessage,"Unsaved changes");};
    }
    void InputCapture_Deactivated(object? sender,EventArgs e) => EndInputCapture(restoreFocus:false);

    IntPtr InputCapture_WindowMessage(IntPtr hwnd,int message,IntPtr wParam,IntPtr lParam,ref bool handled)
    {
        // WPF calls both main Enter and keypad Enter Key.Return. The window's own
        // message preserves the extended-key bit; no global keyboard hook is used.
        if(message is 0x100 or 0x104) keypadEnter=wParam.ToInt32()==0x0D && (lParam.ToInt64() & (1L<<24))!=0;
        return IntPtr.Zero;
    }

    void CaptureBinding_Click(object sender,RoutedEventArgs e)
    {
        if(sender is Button button && button.DataContext is KeyBindingRow row)
            BeginInputCapture(row,button.CommandParameter?.ToString()??"keyboard");
    }
    void BeginInputCapture(KeyBindingRow row,string slot)
    {
        if(slot is not ("keyboard" or "mouse")) return;
        captureFocus=Keyboard.FocusedElement;
        captureRow=row; captureSlot=slot;
        captureModifiers.Clear();
        foreach(var key in InputKeyNames.ModifierKeys)if(Keyboard.IsKeyDown(key))captureModifiers.Add(key);
        CaptureAction.Text=row.Action;
        UpdateCapturePrompt();
        CaptureEscapeButton.Visibility=slot=="keyboard"?Visibility.Visible:Visibility.Collapsed;
        CaptureOverlay.Visibility=Visibility.Visible;
        CaptureOverlay.Focus();
    }
    void EndInputCapture(bool restoreFocus=true)
    {
        if(captureRow==null)return;
        captureRow=null;
        captureModifiers.Clear();
        CaptureOverlay.Visibility=Visibility.Collapsed;
        var previous=captureFocus; captureFocus=null;
        if(restoreFocus && IsActive && previous!=null) Keyboard.Focus(previous);
    }
    void FinishInputCapture(string token)
    {
        if(captureRow==null)return;
        var row=captureRow; string slot=captureSlot;
        EndInputCapture();
        SetBindingSlot(row,slot,token);
    }
    void CancelCapture_Click(object sender,RoutedEventArgs e) => EndInputCapture();
    void ClearCapture_Click(object sender,RoutedEventArgs e) => FinishInputCapture("");
    void EscapeCapture_Click(object sender,RoutedEventArgs e) => FinishInputCapture(InputKeyNames.Chord(captureModifiers,"Escape"));

    void UpdateCapturePrompt()
    {
        if(captureModifiers.Count>0) L10n.SetText(CapturePrompt,captureSlot=="keyboard"?
            "Holding {0}. Press another key, or release a modifier to assign it.":
            "Holding {0}. Press a mouse button or turn the wheel.",
            string.Join(" + ",InputKeyNames.ModifierKeys.Where(captureModifiers.Contains).Select(key=>InputKeyNames.Display(InputKeyNames.Token(key)!))));
        else L10n.SetText(CapturePrompt,captureSlot=="keyboard"?"Press a keyboard key…":"Press a mouse button or turn the wheel…");
    }

    void InputCapture_KeyDown(object sender,KeyEventArgs e)
    {
        if(captureRow==null)return;
        e.Handled=true;
        if(e.IsRepeat)return;
        Key key=e.Key==Key.System?e.SystemKey:e.Key==Key.ImeProcessed?e.ImeProcessedKey:e.Key;
        if(key==Key.Escape) {EndInputCapture();return;}
        if(InputKeyNames.IsModifier(key)) {captureModifiers.Add(key);UpdateCapturePrompt();return;}
        if(captureSlot!="keyboard")return;
        string? token=InputKeyNames.Token(key,keypadEnter);
        if(token==null) {L10n.SetText(CapturePrompt,"This key cannot be assigned. Choose another key, or press Esc to cancel.");return;}
        FinishInputCapture(InputKeyNames.Chord(captureModifiers,token));
    }
    void InputCapture_KeyUp(object sender,KeyEventArgs e)
    {
        if(captureRow==null)return;
        e.Handled=true;
        Key key=e.Key==Key.System?e.SystemKey:e.Key==Key.ImeProcessed?e.ImeProcessedKey:e.Key;
        if(!captureModifiers.Remove(key))return;
        if(captureSlot=="keyboard")FinishInputCapture(InputKeyNames.Chord(captureModifiers,InputKeyNames.Token(key)!));
        else UpdateCapturePrompt();
    }
    static bool IsCaptureCommand(object source)
    {
        for(var node=source as DependencyObject;node!=null;)
        {
            if(node is Button button && button.Tag?.ToString()=="capture-command")return true;
            node=node is Visual?VisualTreeHelper.GetParent(node):LogicalTreeHelper.GetParent(node);
        }
        return false;
    }
    void InputCapture_MouseDown(object sender,MouseButtonEventArgs e)
    {
        if(captureRow==null || IsCaptureCommand(e.OriginalSource))return;
        e.Handled=true;
        if(captureSlot!="mouse")return;
        string token=e.ChangedButton switch
        {
            MouseButton.Left=>"Mouse1", MouseButton.Right=>"Mouse2", MouseButton.Middle=>"Mouse3",
            MouseButton.XButton1=>"Mouse4", MouseButton.XButton2=>"Mouse5", _=>""
        };
        if(token.Length>0)FinishInputCapture(InputKeyNames.Chord(captureModifiers,token));
    }
    void InputCapture_Wheel(object sender,MouseWheelEventArgs e)
    {
        if(captureRow==null)return;
        e.Handled=true;
        if(captureSlot=="mouse" && e.Delta!=0) FinishInputCapture(InputKeyNames.Chord(captureModifiers,e.Delta>0?"WheelUp":"WheelDown"));
    }
}
