' The script of the test package: the language's statements and functions, the installer's session, records and messages.

Function Main()
	Dim parts, i, total, text
	parts = Split("3,4,5", ",")
	total = 0
	For i = 0 To UBound(parts)
		total = total + CInt(parts(i))
	Next
	text = UCase(Left(Session.Property("ProductName"), 5)) & "-" & total
	Select Case total
	Case 11, 12
		text = text & "-twelve"
	Case Else
		text = text & "-other"
	End Select
	On Error Resume Next
	Dim broken
	broken = 1 / 0
	If Err.Number = 11 Then text = text & "-caught"
	On Error GoTo 0
	Dim record
	Set record = Session.Installer.CreateRecord(1)
	record.StringData(1) = "A message from the script"
	Session.Message &H04000000, record
	If Session.EvaluateCondition("ProductName = ""Raven Script""") = 1 Then text = text & "-condition"
	text = text & "-" & Replace(Join(Array("a", "b", "c"), "+"), "b", "B") & "-" & Mid("abcdef", 2, 3) & "-" & InStr("hello", "ll")
	Do While total > 9
		total = total - 1
	Loop
	text = text & "-" & total & "-" & Twice(21)
	Session.Property("SCRIPT_RESULT") = text
	Main = 1
End Function

Function Twice(n)
	Twice = n * 2
End Function

' Deferred: the script sees only the data the installer handed it.
Function CheckData()
	If Session.Property("CustomActionData") = "payload" And Session.Mode(16) Then
		CheckData = 1
	Else
		CheckData = 3
	End If
End Function

' Answers that the installation failed.
Function Refuse()
	Refuse = 3
End Function

' Stops on an error nobody handles.
Function Broken()
	Dim missing
	Set missing = CreateObject("Raven.NoSuchClass")
	Broken = 1
End Function
