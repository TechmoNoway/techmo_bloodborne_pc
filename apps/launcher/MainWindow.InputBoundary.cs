// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.RegularExpressions;

namespace BloodborneLauncher;

public partial class MainWindow
{
    // Techmo's UI uses Mouse2=right, Mouse3=middle. Engine's input files use SDL's
    // Mouse2=middle, Mouse3=right. Apply the involution only at the INI boundary.
    static string SwapEngineMouseTokens(string value) => Regex.Replace(value, @"\bMouse[23]\b",
        match => match.Value == "Mouse2" ? "Mouse3" : "Mouse2");
}
