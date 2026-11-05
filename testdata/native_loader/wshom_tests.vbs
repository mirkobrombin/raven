' The script host's object model, WScript.Shell and WScript.Network, run by cscript. Each Check compares the text of a value with the
' text it should have; the script prints how many checks passed when all of them held.

Option Explicit
Dim failures, checks
failures = 0
checks = 0

Sub Check(actual, expected, label)
	checks = checks + 1
	If CStr(actual) <> CStr(expected) Then
		failures = failures + 1
		WScript.StdErr.WriteLine "FAIL " & label & ": got [" & actual & "], want [" & expected & "]"
	End If
End Sub

Dim shell, network, fso
Set shell = CreateObject("WScript.Shell")
Set network = CreateObject("WScript.Network")
Set fso = CreateObject("Scripting.FileSystemObject")

' The environments.
Dim process
Set process = shell.Environment("Process")
process("RAVEN_WSHOM") = "value"
Check process("RAVEN_WSHOM"), "value", "a process variable set"
Check shell.ExpandEnvironmentStrings("[%RAVEN_WSHOM%]"), "[value]", "ExpandEnvironmentStrings"
Dim variable, seen
seen = False
For Each variable In process
	If variable = "RAVEN_WSHOM=value" Then seen = True
Next
Check seen, True, "For Each over the environment"
process.Remove "RAVEN_WSHOM"
Check process("RAVEN_WSHOM"), "", "a process variable removed"
Check process.Count > 0, True, "Count"

' The registry.
Const key = "HKCU\Software\Raven\WshomTests\"
shell.RegWrite key & "Text", "abc"
shell.RegWrite key & "Number", 42, "REG_DWORD"
shell.RegWrite key, "default"
Check shell.RegRead(key & "Text"), "abc", "a string read back"
Check shell.RegRead(key & "Number"), 42, "a number read back"
Check TypeName(shell.RegRead(key & "Number")), "Long", "a number's type"
Check shell.RegRead(key), "default", "the key's default value"
Check shell.RegRead("HKEY_CURRENT_USER\Software\Raven\WshomTests\Text"), "abc", "the root's long name"
shell.RegDelete key & "Text"
On Error Resume Next
shell.RegRead key & "Text"
Check Err.Number <> 0, True, "a deleted value is gone"
Err.Clear
shell.RegRead "HKXX\Software\Value"
Check Err.Number <> 0, True, "an unknown root"
Err.Clear
On Error GoTo 0
shell.RegDelete key & "Number"
shell.RegDelete key
On Error Resume Next
shell.RegRead key
Check Err.Number <> 0, True, "a deleted key is gone"
Err.Clear
On Error GoTo 0

' Programs.
Dim exec, out
Set exec = shell.Exec("cmd /c echo from exec")
out = exec.StdOut.ReadAll
Do While exec.Status = 0
	WScript.Sleep 20
Loop
Check out, "from exec" & vbCrLf, "the output of Exec"
Check exec.ExitCode, 0, "the exit code of Exec"
Check exec.ProcessID > 0, True, "ProcessID"
Set exec = shell.Exec("cmd /c exit 3")
Do While exec.Status = 0
	WScript.Sleep 20
Loop
Check exec.ExitCode, 3, "an exit code through Exec"
Check shell.Run("cmd /c exit 5", 0, True), 5, "Run waiting for the program"

' The current directory and the special folders.
Dim before
before = shell.CurrentDirectory
shell.CurrentDirectory = fso.GetSpecialFolder(2).Path
Check LCase(shell.CurrentDirectory), LCase(fso.GetSpecialFolder(2).Path), "CurrentDirectory"
shell.CurrentDirectory = before
Check shell.SpecialFolders.Count, 16, "the special folders"
Check shell.SpecialFolders("NoSuchFolder"), "", "a special folder that is not there"

' Shortcuts.
Dim folder, link, again
folder = fso.BuildPath(fso.GetSpecialFolder(2).Path, "raven-wshom-" & fso.GetTempName)
fso.CreateFolder folder
Set link = shell.CreateShortcut(fso.BuildPath(folder, "site.url"))
link.TargetPath = "https://example.com/"
link.Save
Set again = shell.CreateShortcut(fso.BuildPath(folder, "site.url"))
Check again.TargetPath, "https://example.com/", "an Internet shortcut read back"
Set link = shell.CreateShortcut(fso.BuildPath(folder, "tool.lnk"))
link.TargetPath = "C:\Windows\System32\cmd.exe"
link.Arguments = "/c exit"
link.Description = "a tool"
link.Save
Set again = shell.CreateShortcut(fso.BuildPath(folder, "tool.lnk"))
Check LCase(again.TargetPath), "c:\windows\system32\cmd.exe", "a shell link's target read back"
Check again.Arguments & "|" & again.Description, "/c exit|a tool", "a shell link's arguments and description"
Check LCase(again.FullName), LCase(fso.BuildPath(folder, "tool.lnk")), "FullName"
fso.DeleteFolder folder

' Keys that do not parse.
On Error Resume Next
shell.SendKeys "{NOSUCHKEY}"
Check Err.Number, 5, "SendKeys of a key that has no name"
Err.Clear
On Error GoTo 0

' The network.
Check Len(network.ComputerName) > 0, True, "ComputerName"
Check Len(network.UserName) > 0, True, "UserName"
Check Len(network.UserDomain) > 0, True, "UserDomain"
Check network.EnumNetworkDrives.Count Mod 2, 0, "the network drives come in pairs"

If failures = 0 Then
	WScript.Echo checks & " checks passed"
	WScript.Quit 0
End If
WScript.Echo failures & " of " & checks & " checks failed"
WScript.Quit 1
