#Requires AutoHotkey v2.0
#SingleInstance Force
; Your AutoHotkey script. Edit this file, then run AutoHotkey again.
; Ctrl+Alt+H shows this window, Ctrl+Alt+Q quits.
ShowWelcome()
^!h::ShowWelcome()
^!q::ExitApp()
ShowWelcome() {
    g := Gui(, "AutoHotkey")
    g.SetFont("s10")
    g.Add("Text", , "AutoHotkey " A_AhkVersion " is running this script:")
    g.Add("Edit", "w420 r2 ReadOnly", A_ScriptFullPath)
    g.Add("Text", , "Ctrl+Alt+H  show this window`nCtrl+Alt+Q  quit")
    g.Add("Button", "Default w80", "OK").OnEvent("Click", (*) => g.Hide())
    g.Show()
}
