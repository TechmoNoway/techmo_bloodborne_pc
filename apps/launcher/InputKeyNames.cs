// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows.Input;

namespace BloodborneLauncher;

internal static class InputKeyNames
{
    internal const int MaxBindingLength=256;
    internal static readonly Key[] ModifierKeys = {Key.LeftShift,Key.RightShift,Key.LeftCtrl,Key.RightCtrl,Key.LeftAlt,Key.RightAlt};
    internal static bool IsModifier(Key key) => ModifierKeys.Contains(key);
    static readonly string[] ModifierNames = {"Left Shift","Right Shift","Left Ctrl","Right Ctrl","Left Alt","Right Alt"};
    // Names consumed by SDL_GetScancodeFromName, not WPF enum spellings.
    // https://github.com/libsdl-org/SDL/blob/main/src/events/SDL_keymap.c
    static readonly Dictionary<Key, string> Special = new()
    {
        [Key.Return]="Return", [Key.Escape]="Escape", [Key.Back]="Backspace", [Key.Tab]="Tab",
        [Key.Space]="Space", [Key.LeftShift]="Left Shift", [Key.RightShift]="Right Shift",
        [Key.LeftCtrl]="Left Ctrl", [Key.RightCtrl]="Right Ctrl", [Key.LeftAlt]="Left Alt", [Key.RightAlt]="Right Alt",
        [Key.Up]="Up", [Key.Down]="Down", [Key.Left]="Left", [Key.Right]="Right",
        [Key.Insert]="Insert", [Key.Delete]="Delete", [Key.Home]="Home", [Key.End]="End",
        [Key.PageUp]="PageUp", [Key.PageDown]="PageDown", [Key.Capital]="CapsLock",
        [Key.NumLock]="Numlock", [Key.Scroll]="ScrollLock", [Key.PrintScreen]="PrintScreen", [Key.Pause]="Pause",
        [Key.OemMinus]="-", [Key.OemPlus]="=", [Key.OemOpenBrackets]="[", [Key.OemCloseBrackets]="]",
        [Key.OemPipe]="\\", [Key.OemSemicolon]=";", [Key.OemQuotes]="'", [Key.OemTilde]="`",
        [Key.OemPeriod]=".", [Key.OemQuestion]="/", [Key.OemBackslash]="NonUSBackslash",
        [Key.Divide]="Keypad /", [Key.Multiply]="Keypad *", [Key.Subtract]="Keypad -",
        [Key.Add]="Keypad +", [Key.Decimal]="Keypad .", [Key.Apps]="Application"
    };
    static readonly Dictionary<string,string> Canonical = Enum.GetValues<Key>()
        .Select(key => Token(key)).Where(token => token != null).Cast<string>()
        .Append("Keypad Enter").Distinct(StringComparer.OrdinalIgnoreCase)
        .ToDictionary(token => token, token => token, StringComparer.OrdinalIgnoreCase);

    internal static string? Token(Key key, bool keypadEnter = false)
    {
        if (key == Key.Return && keypadEnter) return "Keypad Enter";
        if (key >= Key.A && key <= Key.Z) return key.ToString();
        if (key >= Key.D0 && key <= Key.D9) return ((int)key-(int)Key.D0).ToString();
        if (key >= Key.NumPad0 && key <= Key.NumPad9) return "Keypad " + ((int)key-(int)Key.NumPad0);
        if (key >= Key.F1 && key <= Key.F24) return key.ToString();
        // Comma is the engine's binding separator; Windows keys are OS shortcuts.
        return Special.GetValueOrDefault(key);
    }
    internal static bool IsMouse(string token) => token is "Mouse1" or "Mouse2" or "Mouse3" or "Mouse4" or "Mouse5" or "WheelUp" or "WheelDown";
    internal static bool IsKeyboard(string token) => Canonical.ContainsKey(token);
    // The spaced separator keeps the SDL name "Keypad +" unambiguous.
    internal static string Primary(string chord) => chord.Split(" + ",StringSplitOptions.None).Last().Trim();
    internal static string Chord(IEnumerable<Key> modifiers,string primary) => string.Join(" + ",
        ModifierKeys.Where(key=>modifiers.Contains(key)).Select(key=>Token(key)!).Where(token=>token!=primary).Append(primary));
    internal static string? NormalizeChord(string chord)
    {
        var parts=chord.Split(" + ",StringSplitOptions.None).Select(part=>part.Trim()).ToArray();
        string primary=Primary(chord);
        primary=Canonical.GetValueOrDefault(primary,primary);
        if(!IsKeyboard(primary) && !IsMouse(primary))return null;
        var modifiers=parts.Take(parts.Length-1).Select(part=>ModifierNames.FirstOrDefault(name=>name.Equals(part,StringComparison.OrdinalIgnoreCase))).ToArray();
        if(modifiers.Any(part=>part==null) || modifiers.Distinct().Count()!=modifiers.Length || modifiers.Contains(primary))return null;
        return string.Join(" + ",ModifierNames.Where(name=>modifiers.Contains(name)).Append(primary));
    }
    internal static (string keyboard, string mouse) Split(string binding)
    {
        string keyboard="", mouse="";
        foreach (string token in binding.Split(',').Select(t => t.Trim()).Where(t => t.Length>0 && !t.Equals("none",StringComparison.OrdinalIgnoreCase)))
            if (IsMouse(Primary(token))) mouse=NormalizeChord(token)??token;
            else keyboard=NormalizeChord(token)??token;
        return (keyboard,mouse);
    }
    internal static string Join(string keyboard, string mouse) => keyboard.Length==0 && mouse.Length==0 ? "none" : string.Join(", ",new[]{keyboard,mouse}.Where(t=>t.Length>0));
    internal static string Display(string token) => token.Contains(" + ",StringComparison.Ordinal) ? string.Join(" + ",token.Split(" + ",StringSplitOptions.None).Select(Display)) : token.Length==0 ? L10n.Text("Not assigned") : token switch
    {
        "Mouse1"=>L10n.Text("Left mouse"), "Mouse2"=>L10n.Text("Right mouse"), "Mouse3"=>L10n.Text("Middle mouse"),
        "Mouse4"=>L10n.Text("Mouse side 4"), "Mouse5"=>L10n.Text("Mouse side 5"),
        "WheelUp"=>L10n.Text("Wheel up"), "WheelDown"=>L10n.Text("Wheel down"),
        "Return"=>"Enter", "Escape"=>"Esc", "Space"=>L10n.Text("Spacebar"),
        "Left Shift"=>L10n.Text("Left Shift"), "Right Shift"=>L10n.Text("Right Shift"),
        "Left Ctrl"=>L10n.Text("Left Ctrl"), "Right Ctrl"=>L10n.Text("Right Ctrl"),
        "Left Alt"=>L10n.Text("Left Alt"), "Right Alt"=>L10n.Text("Right Alt"), _=>token
    };
}
