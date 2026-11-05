# Guest VBScript engine

Raven's own vbscript.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It is an Active Scripting engine: hosts create it by the
VBScript class, give it a site and named items through `IActiveScript`, and hand it code through
`IActiveScriptParse`. Values are variants, and oleaut32 does their arithmetic, comparisons and
conversions; objects are called through `IDispatch`.

- `lex.c`: names, numbers, strings, date literals, comments and line continuations.
- `parse.c`: statements, procedures and classes, by recursive descent with VBScript's operator
  precedence.
- `run.c`: the interpreter: variables and ByRef parameters, arrays, calls on objects, classes the
  script defines (as objects with an `IDispatch` of their own), `With`, `On Error Resume Next`.
- `builtins.c`: the string, conversion, math, date, array and formatting functions, the `vb`
  constants, `CreateObject`, `GetRef`, `Eval` and `Execute`.
- `err.c`: the `Err` object.
- `regex.c`: regular expressions in the syntax both script languages share, compiled to a graph of
  nodes and run by a backtracking matcher; it needs nothing but Windows itself.
- `regexp.c`: the `RegExp` object (`New RegExp`, or `VBScript.RegExp` through COM) with its
  `Matches`, `Match` and `SubMatches`.
- `engine.c`: the COM server: `IActiveScript`, `IActiveScriptParse`, `IObjectSafety`, errors
  reported to the site through `IActiveScriptError`, and the script's globals as an `IDispatch`.

Not yet there: `GetObject`, `Execute` in the scope of the calling procedure
(it runs at global level), `AddScriptlet`, `AddTypeLib` and the debugging interfaces.

`exports.txt` lists the exports. See [LICENSES.md](../../../LICENSES.md).
