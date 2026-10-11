using System.IO;
using System.Windows.Controls;
using System.Globalization;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using System.Windows.Data;
using System.Windows.Input;

namespace BloodborneLauncher;
public partial class MainWindow
{
    public sealed class KeyBindingRow : INotifyPropertyChanged
    {
        public string ActionKey { get; set; } = "";
        public string Action => L10n.Text(ActionKey);
        string binding="";
        public string Binding { get => binding; set { if(binding==value)return; binding=value; RefreshLabels(); } }
        public string Key { get; set; } = "";
        public string KeyboardToken => InputKeyNames.Split(Binding).keyboard;
        public string MouseToken => InputKeyNames.Split(Binding).mouse;
        public string KeyboardDisplay => InputKeyNames.Display(KeyboardToken);
        public string MouseDisplay => InputKeyNames.Display(MouseToken);
        public string GroupId => Key.StartsWith("move_") || Key is "walk" or "roll" or "l3" ? "movement" :
            Key.StartsWith("look_") || Key=="lock_on" ? "camera" :
            Key is "attack" or "strong_attack" or "strong_modifier" or "transform" or "firearm" or "blood_vial" or "use_item" ? "combat" : "menu";
        public string Group => L10n.Text(GroupId switch {"movement"=>"Movement", "combat"=>"Combat", "camera"=>"Camera", _=>"Items & menus"});
        public int GroupOrder => GroupId switch {"movement"=>0,"combat"=>1,"camera"=>2,_=>3};
        public int Order => Array.FindIndex(PcDefaults,entry=>entry.key==Key);
        public event PropertyChangedEventHandler? PropertyChanged;
        public void RefreshLabels() => PropertyChanged?.Invoke(this,new PropertyChangedEventArgs(""));
    }
    readonly ObservableCollection<KeyBindingRow> keyBindings = new();
    ICollectionView? bindingView;
    bool updatingBindings;
    static readonly (string key,string label,string binding)[] PcDefaults =
    {
        ("move_forward","Move forward","W"),("move_back","Move backward","S"),("move_left","Move left","A"),("move_right","Move right","D"),
        ("walk","Walk (hold)","Left Alt"),("roll","Dodge / sprint","Space"),("lock_on","Lock on","Q"),
        ("attack","Attack","Mouse1"),("strong_attack","Strong attack","V"),("strong_modifier","Strong attack modifier","Left Shift"),
        ("transform","Transform weapon","C, Mouse3"),("firearm","Firearm / parry","X, Mouse2"),
        ("blood_vial","Blood vial","R"),("use_item","Use item","F"),("interact","Interact","E"),
        ("switch_item","Switch item","Down"),("switch_right","Switch right-hand weapon","Right"),("switch_left","Switch left-hand weapon","Left"),
        ("dpad_up","D-pad up","Up"),("gestures","Gestures","G"),("personal_effects","Personal effects","T"),
        ("look_up","Look up","I"),("look_down","Look down","K"),("look_left","Look left","J"),("look_right","Look right","L"),
        ("menu","Menu","Escape"),("confirm","Confirm in menus","Return"),("back","Back in menus","Backspace"),("l3","Left stick click","Left Ctrl")
    };
    void LoadInputState()
    {
        state.PcControls=ReadIniValue("pc_controls")!="0";
        state.Widescreen=ReadIniValue("widescreen")!="0";
        state.MouseCamera=ReadIniValue("mouse_camera")!="0";
        state.MouseMenu=ReadIniValue("mouse_menu")!="0";
        state.MouseInvertX=ReadIniValue("mouse_invert_x")=="1";
        state.MouseInvertY=ReadIniValue("mouse_invert_y")=="1";
        state.MouseAutoRotation=ReadIniValue("mouse_auto_rotation")=="1";
        state.MouseSensitivity=int.TryParse(ReadIniValue("mouse_sensitivity"),out int n)?Math.Clamp(n,0,10):5;
        keyBindings.Clear();
        foreach(var d in PcDefaults)keyBindings.Add(new(){Key=d.key,ActionKey=d.label,Binding=ReadIniValue("bind."+d.key)??d.binding});
        if(state.CustomInputBindings!=null && !HasCustomBindings())state.CustomInputBindings=null;
        RememberCustomBindings();
    }
    void ApplyInputUi()
    {
        PcControlsCheck.IsChecked=state.PcControls;WidescreenCheck.IsChecked=state.Widescreen;
        MouseCameraCheck.IsChecked=state.MouseCamera;MouseMenuCheck.IsChecked=state.MouseMenu;
        MouseInvertXCheck.IsChecked=state.MouseInvertX;MouseInvertYCheck.IsChecked=state.MouseInvertY;
        MouseAutoRotationCheck.IsChecked=state.MouseAutoRotation;MouseSensitivitySlider.Value=state.MouseSensitivity;
        bindingView=CollectionViewSource.GetDefaultView(keyBindings);
        if (bindingView.GroupDescriptions.Count==0) bindingView.GroupDescriptions.Add(new PropertyGroupDescription(nameof(KeyBindingRow.Group)));
        if (bindingView.SortDescriptions.Count==0)
        {
            bindingView.SortDescriptions.Add(new SortDescription(nameof(KeyBindingRow.GroupOrder),ListSortDirection.Ascending));
            bindingView.SortDescriptions.Add(new SortDescription(nameof(KeyBindingRow.Order),ListSortDirection.Ascending));
        }
        bindingView.Filter=item => item is KeyBindingRow row && (ComboTag(BindingGroupCombo,"all")=="all" || row.GroupId==ComboTag(BindingGroupCombo));
        KeyBindingsGrid.ItemsSource=bindingView;
        RefreshBindings();
    }
    void CollectInputUi()
    {
        KeyBindingsGrid.CommitEdit();KeyBindingsGrid.CommitEdit(DataGridEditingUnit.Row,true);
        state.PcControls=PcControlsCheck.IsChecked==true;state.Widescreen=WidescreenCheck.IsChecked==true;
        state.MouseCamera=MouseCameraCheck.IsChecked==true;state.MouseMenu=MouseMenuCheck.IsChecked==true;
        state.MouseInvertX=MouseInvertXCheck.IsChecked==true;state.MouseInvertY=MouseInvertYCheck.IsChecked==true;
        state.MouseAutoRotation=MouseAutoRotationCheck.IsChecked==true;state.MouseSensitivity=(int)MouseSensitivitySlider.Value;
        RememberCustomBindings();
    }
    void SaveInputSettings(Dictionary<string,string> ini)
    {
        ini["pc_controls"]=state.PcControls?"1":"0";ini["widescreen"]=state.Widescreen?"1":"0";
        ini["mouse_camera"]=state.MouseCamera?"1":"0";ini["mouse_menu"]=state.MouseMenu?"1":"0";
        ini["mouse_invert_x"]=state.MouseInvertX?"1":"0";ini["mouse_invert_y"]=state.MouseInvertY?"1":"0";
        ini["mouse_auto_rotation"]=state.MouseAutoRotation?"1":"0";
        ini["mouse_sensitivity"]=state.MouseSensitivity.ToString(CultureInfo.InvariantCulture);
        foreach(var row in keyBindings)
        {
            string v=row.Binding.Trim();
            if(v.Length>InputKeyNames.MaxBindingLength || v.Any(c=>c=='\r' || c=='\n') || v.Split(',').Any(slot=>slot.Contains(" + ",StringComparison.Ordinal) && InputKeyNames.NormalizeChord(slot.Trim())==null))throw new InvalidOperationException(L10n.Text("Invalid key binding: {0}", row.Action));
            ini["bind."+row.Key]=v.Length==0?"none":v;
        }
    }
    void ResetKeys_Click(object sender,System.Windows.RoutedEventArgs e)
    {
        ApplyBindingPreset("default");
    }

    static string PresetBinding(string key,string defaults,string preset) => preset=="ds3" ? key switch
    {
        "lock_on"=>"Q, Mouse3", "transform"=>"C", "switch_item"=>"Down, WheelDown", _=>defaults
    } : defaults;

    void ApplyBindingPreset(string preset)
    {
        if(preset is not ("default" or "ds3" or "custom") || preset=="custom" && !HasCustomBindings())return;
        RememberCustomBindings();
        foreach(var d in PcDefaults) keyBindings.First(row=>row.Key==d.key).Binding=preset=="custom"?state.CustomInputBindings![d.key]:PresetBinding(d.key,d.binding,preset);
        RefreshBindings(regroup:false);
        L10n.SetText(FooterMessage,"Unsaved changes");
    }
    void BindingPreset_Changed(object sender,SelectionChangedEventArgs e)
    {
        if(syncing || updatingBindings || keyBindings.Count==0)return;
        ApplyBindingPreset(ComboTag(BindingPresetCombo,"custom"));
    }
    void BindingGroup_Changed(object sender,SelectionChangedEventArgs e) => bindingView?.Refresh();
    bool HasCustomBindings() => state.CustomInputBindings is {} values && values.Count==PcDefaults.Length && PcDefaults.All(d=>values.TryGetValue(d.key,out var value) && value!=null && value.Length<=InputKeyNames.MaxBindingLength && !value.Any(c=>c=='\r' || c=='\n'));
    string CurrentBindingPreset() => new[]{"default","ds3"}.FirstOrDefault(p => PcDefaults.All(d =>
        keyBindings.First(row=>row.Key==d.key).Binding==PresetBinding(d.key,d.binding,p)))??"custom";
    void RememberCustomBindings()
    {
        if(keyBindings.Count==PcDefaults.Length && CurrentBindingPreset()=="custom")
            state.CustomInputBindings=keyBindings.ToDictionary(row=>row.Key,row=>row.Binding);
    }
    void RefreshBindings(bool regroup=true)
    {
        updatingBindings=true;
        try
        {
            foreach(var row in keyBindings) row.RefreshLabels();
            if(regroup) bindingView?.Refresh();
            CustomBindingPreset.IsEnabled=HasCustomBindings();
            SetComboTag(BindingPresetCombo,CurrentBindingPreset());
            var shared=keyBindings.SelectMany(row=>new[]{row.KeyboardToken,row.MouseToken}.Where(t=>t.Length>0).Select(token=>(token,row.Action)))
                .GroupBy(p=>p.token,StringComparer.OrdinalIgnoreCase).Where(g=>g.Count()>1)
                .Select(g=>InputKeyNames.Display(g.Key)+" ("+string.Join(", ",g.Select(p=>p.Action))+")").ToArray();
            BindingConflictText.Visibility=shared.Length==0?Visibility.Collapsed:Visibility.Visible;
            L10n.SetText(BindingConflictText,"Shared inputs: {0}. These actions will respond together.",string.Join("; ",shared));
        }
        finally {updatingBindings=false;}
    }
    void ClearBinding_Click(object sender,RoutedEventArgs e)
    {
        if(sender is not Button button || button.DataContext is not KeyBindingRow row)return;
        SetBindingSlot(row,button.CommandParameter?.ToString()??"keyboard","");
    }
    void SetBindingSlot(KeyBindingRow row,string slot,string token)
    {
        var current=InputKeyNames.Split(row.Binding);
        row.Binding=InputKeyNames.Join(slot=="keyboard"?token:current.keyboard,slot=="mouse"?token:current.mouse);
        RememberCustomBindings();
        RefreshBindings(regroup:false);
        L10n.SetText(FooterMessage,"Unsaved changes");
    }
}
