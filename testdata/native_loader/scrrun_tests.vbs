' The script host and the scripting runtime: run by cscript with the arguments first /named:value "two words". Each Check compares
' the text of a value with the text it should have; the script quits with 7 when all of them held and with 1 otherwise.

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

' The host.
Check WScript.Arguments.Count, 3, "argument count"
Check WScript.Arguments(0), "first", "first argument"
Check WScript.Arguments(2), "two words", "a quoted argument"
Check WScript.Arguments.Named("named"), "value", "a named argument"
Check WScript.Arguments.Named.Exists("NAMED"), True, "named arguments ignore case"
Check WScript.Arguments.Unnamed.Count, 2, "unnamed arguments"
Check WScript.ScriptName, "scrrun_tests.vbs", "ScriptName"
Check LCase(Right(WScript.FullName, 11)), "cscript.exe", "FullName"
Check WScript.Name, "Windows Script Host", "Name"
Dim joined, argument
For Each argument In WScript.Arguments
	joined = joined & argument & ";"
Next
Check joined, "first;/named:value;two words;", "For Each over the arguments"
WScript.StdOut.Write "out "
WScript.StdOut.WriteLine "line"
WScript.Echo "echo", 1, True

' Scripting.Dictionary.
Dim d, keys
Set d = CreateObject("Scripting.Dictionary")
d.Add "one", 1
d.Add "two", 2
d("three") = 3
Check d.Count, 3, "Count"
Check d("two"), 2, "Item by key"
Check d.Exists("one") & d.Exists("ONE"), "TrueFalse", "keys compare exactly"
keys = d.Keys
Check Join(keys, ","), "one,two,three", "Keys keep their order"
Check Join(d.Items, ","), "1,2,3", "Items"
d.Remove "two"
Check Join(d.Keys, ","), "one,three", "Remove"
d.Key("three") = "drei"
Check d("drei"), 3, "Key renames"
On Error Resume Next
d.Add "one", 9
Check Err.Number, 457, "Add of a key already there"
Err.Clear
d.Remove "absent"
Check Err.Number, 32811, "Remove of a key that is not there"
Err.Clear
On Error GoTo 0
Dim t
Set t = CreateObject("Scripting.Dictionary")
t.CompareMode = 1
t.Add "Key", "v"
Check t.Exists("KEY"), True, "text compare mode"
t.Add 1, "number"
Check t(1.0), "number", "numbers by value"
Dim k, listed
For Each k In d
	listed = listed & k & ";"
Next
Check listed, "one;drei;", "For Each over the keys"
Check IsEmpty(d("new")), True, "reading a missing item"
Check d.Exists("new"), True, "which adds it"
d.RemoveAll
Check d.Count, 0, "RemoveAll"

' Scripting.FileSystemObject.
Dim fso, root, stream, f, folder, item, names
Set fso = CreateObject("Scripting.FileSystemObject")
Check fso.BuildPath("C:\a", "b.txt"), "C:\a\b.txt", "BuildPath"
Check fso.GetFileName("C:\a\b.txt"), "b.txt", "GetFileName"
Check fso.GetBaseName("C:\a\b.txt"), "b", "GetBaseName"
Check fso.GetExtensionName("C:\a\b.tar.gz"), "gz", "GetExtensionName"
Check fso.GetParentFolderName("C:\a\b\c.txt"), "C:\a\b", "GetParentFolderName"
Check fso.GetParentFolderName("C:\a"), "C:\", "the parent of a top folder"
Check fso.GetDriveName("C:\a\b"), "C:", "GetDriveName"
root = fso.BuildPath(fso.GetSpecialFolder(2).Path, "raven-scrrun-" & fso.GetTempName)
fso.CreateFolder root
Check fso.FolderExists(root), True, "CreateFolder"
Set stream = fso.CreateTextFile(fso.BuildPath(root, "a.txt"))
stream.WriteLine "first line"
stream.Write "second"
stream.WriteBlankLines 1
stream.Close
Set stream = fso.OpenTextFile(fso.BuildPath(root, "a.txt"), 8)
stream.WriteLine "third"
stream.Close
Set stream = fso.OpenTextFile(fso.BuildPath(root, "a.txt"))
Check stream.ReadLine, "first line", "ReadLine"
Check stream.Line, 2, "Line"
Check stream.Read(3), "sec", "Read"
Check stream.ReadLine, "ond", "the rest of a line"
Check stream.ReadAll, "third" & vbCrLf, "ReadAll"
Check stream.AtEndOfStream, True, "AtEndOfStream"
stream.Close
Set stream = fso.CreateTextFile(fso.BuildPath(root, "u.txt"), True, True)
stream.Write "wide " & ChrW(&H3A9)
stream.Close
Set f = fso.GetFile(fso.BuildPath(root, "u.txt"))
Check f.Size, 14, "a UTF-16 file has its mark"
Check f.OpenAsTextStream(1, -1).ReadAll, "wide " & ChrW(&H3A9), "UTF-16 read back"
Check f.Name & " " & f.ParentFolder.Name, "u.txt " & fso.GetFileName(root), "File names"
fso.CreateFolder fso.BuildPath(root, "sub")
fso.CopyFile fso.BuildPath(root, "*.txt"), fso.BuildPath(root, "sub") & "\"
Set folder = fso.GetFolder(fso.BuildPath(root, "sub"))
Check folder.Files.Count, 2, "CopyFile with a wildcard"
For Each item In folder.Files
	names = names & item.Name & ";"
Next
Check InStr(names, "a.txt;") > 0 And InStr(names, "u.txt;") > 0, True, "the Files collection"
Check fso.GetFolder(root).SubFolders.Count, 1, "SubFolders"
fso.MoveFile fso.BuildPath(root, "sub\a.txt"), fso.BuildPath(root, "moved.txt")
Check fso.FileExists(fso.BuildPath(root, "moved.txt")) & fso.FileExists(fso.BuildPath(root, "sub\a.txt")), "TrueFalse", "MoveFile"
On Error Resume Next
fso.GetFile fso.BuildPath(root, "absent.txt")
Check Err.Number, 53, "GetFile of a file that is not there"
Err.Clear
fso.CopyFile fso.BuildPath(root, "a.txt"), fso.BuildPath(root, "moved.txt"), False
Check Err.Number, 58, "CopyFile that would overwrite"
Err.Clear
On Error GoTo 0
fso.DeleteFolder root
Check fso.FolderExists(root), False, "DeleteFolder takes the tree"

If failures = 0 Then
	WScript.Echo checks & " checks passed"
	WScript.Quit 7
End If
WScript.Echo failures & " of " & checks & " checks failed"
WScript.Quit 1
