# Guest script host object model

Raven's own wshom.ocx for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers. It makes `WScript.Shell` and `WScript.Network` through COM. Its
objects are the scripting runtime's dispatch objects, compiled in from
[raven-scrrun](../raven-scrrun/README.md) (`object.c`), and the streams of `Exec` are the
runtime's text streams (`textstream.c`).

- `main.c`: the class factories, the exports, and the read-only collection that
  `SpecialFolders` and the network's lists give.
- `shell.c`: `WshShell`: `Run` through the shell, `Popup`, `ExpandEnvironmentStrings`,
  `SpecialFolders`, `AppActivate`, `LogEvent` and `CurrentDirectory`.
- `exec.c`: `Exec` and `WshScriptExec`, a program with its standard streams on pipes.
- `registry.c`: `RegRead`, `RegWrite`, `RegDelete`, and `Environment` for the process, the
  system, the user and the session.
- `shortcut.c`: `CreateShortcut`: shell links (`.lnk`) through `IShellLinkW`, and Internet
  shortcuts (`.url`).
- `keys.c`: `SendKeys`, typed with `SendInput`.
- `network.c`: `WshNetwork`: the computer, user and domain names, and network drives and
  printers through mpr.dll and winspool.drv, looked up when they are used.

Not yet there: the early-bound interfaces with type information, `WshController` and remote
scripts, and `Popup`'s timeout where user32 has no `MessageBoxTimeoutW`.

`exports.txt` lists the exports. See [LICENSES.md](../../../LICENSES.md).
