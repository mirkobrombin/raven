' The language checks vbscript_client.c runs: each Check compares the text of a value with the text it should have.

Check 1 + 2 * 3, "7", "precedence"
Check 2 ^ 3 ^ 2, "64", "power is left-associative"
Dim negated
negated = -2 ^ 2
Check negated, "-4", "power before negation"
Check 7 \ 2, "3", "integer division"
Check 7 Mod 3, "1", "modulo"
Check 7 / 2, "3.5", "division"
Check "a" & 1 & True, "a1True", "concatenation"
Check "1" + 2, "3", "a numeric string added"
Check "a" + "b", "ab", "strings added"
Check TypeName(1), "Integer", "small literal"
Check TypeName(40000), "Long", "large literal"
Check TypeName(1.5), "Double", "real literal"
Check TypeName(32767 + 1), "Long", "Integer overflow widens"
Check &HFF, "255", "hexadecimal"
Check &HFFFF, "-1", "hexadecimal Integer bits"
Check &H10&, "16", "hexadecimal Long"
Check &O17, "15", "octal"
Check 3 > 2 And 2 > 1, "True", "comparison and And"
Check Not 0, "-1", "Not of a number"
Check 6 And 3, "2", "bitwise And"
Check 5 Or 2, "7", "bitwise Or"
Check 5 Xor 1, "4", "Xor"
Check "abc" < "abd", "True", "string order"
Check 1 < "a", "True", "a number before any string"
Check IsNull(Null + 1), "True", "Null propagates"
Check IsEmpty(never_set), "True", "an undeclared variable is Empty"
Check Len(Empty), "0", "Len of Empty"
Check CInt(2.5), "2", "CInt rounds to even"
Check CInt(3.5), "4", "CInt rounds to even upward"
Check Round(2.567, 2), "2.57", "Round"
Check Int(-2.5), "-3", "Int"
Check Fix(-2.5), "-2", "Fix"
Check Abs(-4), "4", "Abs"
Check Sgn(-4), "-1", "Sgn"
Check Sqr(16), "4", "Sqr"
Check Hex(255), "FF", "Hex"
Check Hex(-1), "FFFF", "Hex of an Integer"
Check Oct(8), "10", "Oct"
Check CStr(True), "True", "CStr of a Boolean"
Check CBool("true"), "True", "CBool of text"
Check CLng("42"), "42", "CLng of text"

' Strings.
Check Len("hello"), "5", "Len"
Check Left("hello", 2), "he", "Left"
Check Right("hello", 3), "llo", "Right"
Check Mid("hello", 2, 3), "ell", "Mid"
Check Mid("hello", 4), "lo", "Mid to the end"
Check InStr("hello", "l"), "3", "InStr"
Check InStr(4, "hello", "l"), "4", "InStr from a start"
Check InStr(1, "HELLO", "l", vbTextCompare), "3", "InStr ignoring case"
Check InStrRev("hello", "l"), "4", "InStrRev"
Check UCase("abc") & LCase("DEF"), "ABCdef", "case"
Check "[" & Trim("  x  ") & "][" & LTrim(" y") & "][" & RTrim("z ") & "]", "[x][y][z]", "trims"
Check Replace("a-b-c", "-", "+"), "a+b+c", "Replace"
Check Replace("a-b-c", "-", "+", 1, 1), "a+b-c", "Replace a count"
Check Replace("aXbxc", "x", "", 1, -1, vbTextCompare), "abc", "Replace ignoring case"
Check StrComp("a", "B", vbTextCompare), "-1", "StrComp"
Check StrReverse("abc"), "cba", "StrReverse"
Check Space(3) & "|", "   |", "Space"
Check String(3, "z"), "zzz", "String"
Check Asc("A") & " " & Chr(66) & " " & ChrW(67) & " " & AscW("D"), "65 B C 68", "character codes"
Check "say ""hi""", "say " & Chr(34) & "hi" & Chr(34), "doubled quotes"
Check Len(vbCrLf), "2", "vbCrLf"

' Arrays.
Dim list(2)
list(0) = "a" : list(1) = "b" : list(2) = "c"
Check UBound(list), "2", "UBound"
Check Join(list, ","), "a,b,c", "Join"
Dim parts
parts = Split("x;y;z", ";")
Check UBound(parts) & parts(1), "2y", "Split"
Check UBound(Split("")), "-1", "Split of nothing"
Check UBound(Split("a,b,c", ",", 2)), "1", "Split a count"
Dim grid(1, 2)
grid(1, 2) = 12
Check grid(1, 2) & " " & UBound(grid, 2), "12 2", "two dimensions"
Dim grow()
ReDim grow(1)
grow(0) = "kept"
ReDim Preserve grow(3)
Check grow(0) & UBound(grow), "kept3", "ReDim Preserve"
Dim made
made = Array(1, "two", 3)
Check TypeName(made) & made(1), "Variant()two", "Array"
Check IsArray(made), "True", "IsArray"
Check Join(Filter(Array("apple", "berry", "grape"), "ap")), "apple grape", "Filter"

' Control flow.
Dim total, i
total = 0
For i = 1 To 10
	total = total + i
Next
Check total, "55", "For"
total = 0
For i = 10 To 1 Step -3
	total = total + i
Next
Check total, "22", "For with a negative step"
total = ""
Dim item
For Each item In Array("p", "q")
	total = total & item
Next
Check total, "pq", "For Each"
i = 0
Do While i < 5
	i = i + 1
Loop
Check i, "5", "Do While"
i = 0
Do
	i = i + 2
Loop Until i >= 7
Check i, "8", "Do Until at the end"
i = 0
While i < 3
	i = i + 1
Wend
Check i, "3", "While"
For i = 1 To 100
	If i = 4 Then Exit For
Next
Check i, "4", "Exit For"
i = 0
Do
	i = i + 1
	If i = 6 Then Exit Do
Loop
Check i, "6", "Exit Do"

Function Grade(score)
	If score >= 90 Then
		Grade = "A"
	ElseIf score >= 80 Then
		Grade = "B"
	Else
		Grade = "C"
	End If
End Function
Check Grade(95) & Grade(85) & Grade(10), "ABC", "If and ElseIf"

Function Kind(value)
	Select Case value
		Case 1, 2
			Kind = "small"
		Case "x"
			Kind = "letter"
		Case Else
			Kind = "other"
	End Select
End Function
Check Kind(2) & Kind("x") & Kind(9), "smallletterother", "Select Case"
If 1 = 1 Then total = "one line" Else total = "no"
Check total, "one line", "one-line If"

' Procedures.
Function Factorial(n)
	If n <= 1 Then
		Factorial = 1
	Else
		Factorial = n * Factorial(n - 1)
	End If
End Function
Check Factorial(10), "3628800", "recursion"

Sub Bump(ByRef target, ByVal amount)
	target = target + amount
	amount = 0
End Sub
Dim counter, step_size
counter = 1
step_size = 5
Bump counter, step_size
Check counter & " " & step_size, "6 5", "ByRef and ByVal"
Call Bump(counter, 1)
Check counter, "7", "Call"
Bump (counter), 1
Check counter, "7", "a parenthesized argument passes a copy"

Sub Early(out)
	out = "before"
	Exit Sub
	out = "after"
End Sub
Early total
Check total, "before", "Exit Sub"

Function Twice(x)
	Twice = x * 2
End Function

Const Limit = 10, Label = "max"
Check Label & Limit, "max10", "Const"

' Classes.
Class Account
	Private balance_
	Public Owner

	Private Sub Class_Initialize()
		balance_ = 100
	End Sub

	Public Property Get Balance()
		Balance = balance_
	End Property

	Public Property Let Balance(value)
		If value < 0 Then Err.Raise 5
		balance_ = value
	End Property

	Public Default Function Describe()
		Describe = Owner & ":" & balance_
	End Function

	Public Function Deposit(amount)
		Me.Balance = Me.Balance + amount
		Set Deposit = Me
	End Function
End Class

Dim acct
Set acct = New Account
acct.Owner = "ann"
Check acct.Balance, "100", "Class_Initialize"
acct.Balance = 150
Check acct.Balance, "150", "Property Let and Get"
Check acct.Deposit(10).Deposit(5).Balance, "165", "methods returning Me"
Check acct, "ann:165", "default member"
Check TypeName(acct), "Account", "TypeName of a class"
Check IsObject(acct), "True", "IsObject"
Dim other
Set other = acct
Check other Is acct, "True", "Is"
Set other = Nothing
Check other Is Nothing, "True", "Nothing"
With acct
	.Owner = "bob"
	Check .Owner, "bob", "With"
End With

Dim terminated
terminated = False
Class Tracker
	Private Sub Class_Terminate()
		terminated = True
	End Sub
End Class
Dim watched
Set watched = New Tracker
Set watched = Nothing
Check terminated, "True", "Class_Terminate"

' Errors.
On Error Resume Next
i = 1 / 0
Check Err.Number, "11", "division by zero"
Err.Clear
Check Err.Number, "0", "Err.Clear"
acct.Balance = -1
Check Err.Number, "5", "an error raised in a class"
Err.Clear
Err.Raise vbObjectError + 7, "tests", "custom"
Check Err.Number = vbObjectError + 7, "True", "Err.Raise of an object error"
Check Err.Source & " " & Err.Description, "tests custom", "Err.Source and Description"
Err.Clear
Set other = CreateObject("No.Such.Class")
Check Err.Number, "429", "CreateObject of an unknown class"
Err.Clear
Host.Fail
Check Hex(Err.Number), "80070005", "an error of a host object"
Check Err.Description, "denied", "the description a host object gives"
On Error GoTo 0

' The host's objects and code made at run time.
Check Host.Add(2, 3), "5", "a host method"
Check Add(4, 5), "9", "a global member of a host item"
Host.Value = "stored"
Check Host.Value, "stored", "a host property"
Execute "Dim made_later : made_later = 6 * 7"
Check made_later, "42", "Execute"
Check Eval("Twice(4) + 1"), "9", "Eval"
Check Year(DateSerial(2024, 13, 1)), "2025", "DateSerial runs over"
Check Month(#3/15/2020#) & "-" & Day(#3/15/2020#), "3-15", "date literal"
Check DateDiff("d", #1/1/2020#, #3/1/2020#), "60", "DateDiff in days"
Check Weekday(#1/1/2020#), "4", "Weekday"
Check VarType("x") & " " & VarType(1) & " " & VarType(Null), "8 2 1", "VarType"
Check IsNumeric("12.5") & IsNumeric("x"), "TrueFalse", "IsNumeric"

' Regular expressions.
Dim re, found, item, joined
Set re = New RegExp
re.Pattern = "(\w+)@(\w+)\.com"
Check re.Test("mail bob@example.com now"), "True", "RegExp.Test"
Set found = re.Execute("mail bob@example.com now")
Check found.Count, "1", "Execute without Global finds one"
Check found(0).Value & " " & found(0).FirstIndex & " " & found(0).Length, "bob@example.com 5 15", "a Match"
Check found(0).SubMatches(0) & "/" & found(0).SubMatches(1), "bob/example", "SubMatches"
re.Pattern = "\d+"
re.Global = True
Set found = re.Execute("a1 b22 c333")
joined = ""
For Each item In found
	joined = joined & item & ","
Next
Check joined, "1,22,333,", "For Each over a global Execute"
Check re.Replace("a1 b22", "#"), "a# b#", "global Replace"
re.Pattern = "(\w+) (\w+)"
re.Global = False
Check re.Replace("one two three", "$2 $1 [$&]"), "two one [one two] three", "Replace with groups"
re.Pattern = "^b.*$"
re.Multiline = True
re.Global = True
Check re.Execute("abc" & vbLf & "bcd" & vbLf & "bed").Count, "2", "Multiline anchors"
re.Pattern = "HELLO"
re.IgnoreCase = True
Check re.Test("say hello"), "True", "IgnoreCase"
re.IgnoreCase = False
Check re.Test("say hello"), "False", "IgnoreCase off again"
re.Pattern = "a(?=b)|c(?!d)"
Check re.Replace("ab ac cd ce", "_"), "_b a_ cd _e", "lookahead"
re.Pattern = "(['""])(.*?)\1"
Check re.Execute("say 'hi' and ""yo""")(1).SubMatches(1), "yo", "back reference and lazy repeat"
re.Pattern = "[^a-c\s]{2,3}"
Check re.Replace("abxyzw cd", "-"), "ab-w cd", "class, negation and counted repeat"
re.Pattern = "(a|ab)(c|bcd)(d*)"
re.Global = False
Check re.Execute("abcd")(0).Value, "abcd", "alternation backtracks"
re.Pattern = "x*"
re.Global = True
Check re.Replace("abc", "-"), "-a-b-c-", "empty matches advance"
Set other = CreateObject("VBScript.RegExp")
other.Pattern = "o"
other.Global = True
Check other.Replace("foo", "0"), "f00", "VBScript.RegExp through CreateObject"
On Error Resume Next
re.Pattern = "(unclosed"
re.Test "x"
Check Err.Number, "5020", "a pattern with an open group"
Err.Clear
On Error GoTo 0

' GetRef: a procedure as an object.
Dim handler
Set handler = GetRef("Twice")
Check handler(8), "16", "GetRef calls the procedure"

Dim Greeting
Greeting = "hi"
Host.Done
