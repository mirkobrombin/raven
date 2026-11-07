# Guest JScript engine

Raven's own jscript.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It is an Active Scripting engine for the language of
ECMAScript 3 with the additions of JScript: hosts create it by the JScript class (or the
`JScript`, `JavaScript` and `ECMAScript` names), give it a site and named items through
`IActiveScript`, and hand it code through `IActiveScriptParse`. Values are the language's own;
they become variants only on the way to and from the host's objects.

- `parse.c`: the lexer and a recursive descent parser to a syntax tree, with automatic semicolons,
  regular expression literals and labels.
- `run.c`: the interpreter: scope chains, closures, `this`, `arguments`, `eval` in the caller's
  context, `with`, exceptions, the conversions and operators of the language, and the names of the
  host's items.
- `object.c`: objects and their properties, arrays with dense and sparse elements, enumeration.
- `dispatch.c`: variants, the host's objects called through `IDispatch` and `IDispatchEx`, and
  script objects handed to the host as `IDispatchEx`.
- `builtins.c`: `Object`, `Function`, `Boolean`, the error objects with JScript's `number` and
  `description`, `Math` and the global functions.
- `array.c`, `string.c`, `number.c`, `date.c`, `regexp.c`: the other built-in objects; regular
  expressions run on the VBScript engine's matcher (`../raven-vbscript/regex.c`).
- `numconv.c`: numbers to decimal digits and back, exactly, on integers of a few thousand bits:
  the shortest digits that read back as the same number, the exact digits `toFixed` and its kin
  round, and decimal text to the nearest double.
- `host.c`: `ActiveXObject`, `GetObject`, `Enumerator` and `VBArray`.
- `engine.c`: the COM server: `IActiveScript`, `IActiveScriptParse`, `IObjectSafety`, errors
  reported to the site through `IActiveScriptError`, and the global object as what
  `GetScriptDispatch` hands out.

Not yet there: conditional compilation (`@cc_on`), `AddScriptlet`, `AddTypeLib`,
`IActiveScriptParseProcedure` and the debugging interfaces.

`exports.txt` lists the exports. See [LICENSES.md](../../../LICENSES.md).
