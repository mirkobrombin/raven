# Command-line tools

The programs a Windows command prompt has beside `cmd.exe`, one program each as Windows has them: `find`, `findstr`, `more` (a
`.com`, as Windows names it), `sort`, `where`, `xcopy`, `attrib`, `timeout`, `choice`, `whoami`, `hostname`, `tasklist`, `taskkill`,
`reg` and `rundll32`. CMake builds each with MinGW for both widths, on the public Windows headers and without a C runtime; Raven copies them into
every prefix's `System32` and `SysWOW64`.

They are Raven code written from Microsoft's public description of each tool and from how they behave; no Windows code is used or
consulted, nor the tools of other projects. `tool.h` holds what they share (text, output, the command line split as Windows splits
it, reading input) and `processes.h` the process list `tasklist` and `taskkill` use.

What each does:

- `find`: `/V /C /N /I`, files or the input, the `----------` headings, and ERRORLEVEL 0, 1 or 2;
- `findstr`: `/B /E /L /R /S /I /X /V /N /M /O /P /C: /G: /F:`, its regular expressions (`. * ^ $ [class] [^class] \< \>`), switches run
  together as `/SI`;
- `more`: a screen at a time on a console with Space, Enter and Q, keys from the console when the text comes from a pipe; `/C /S /P
  /Tn +n`;
- `sort`: `/R /+n /C /O`, a stable sort that ignores case;
- `where`: the current folder and `PATH` with `PATHEXT`, `/R`, `$VAR:pattern`, `dir:pattern`, `/Q /F /T`;
- `xcopy`: `/S /E /I /Q /F /L /Y /-Y /H /R /K /C /D[:date] /T /U /A /M /P /W /EXCLUDE:`, and the file-or-folder question;
- `attrib`: `+-R A S H I O`, `/S /D`;
- `timeout`: `/T`, `/NOBREAK`, -1, a count on the console, and refusing redirected input as Windows does;
- `choice`: `/C /N /CS /T /D /M`, ERRORLEVEL the choice's position;
- `whoami` (`/USER`, `/FO`, `/NH`) and `hostname`;
- `tasklist` (`/FI` on IMAGENAME, PID, MEMUSAGE and SESSION; `/FO TABLE | LIST | CSV`; `/NH`) and `taskkill` (`/PID`, `/IM` with
  wildcards, `/FI`, `/T`, `/F`; without `/F` a process's windows are asked to close, and one with no window can only be ended with
  `/F`. Raven's windows belong to the process that made them, so for now `taskkill` sees only its own and another process needs `/F`);
- `rundll32`: `library,entry [arguments]`, the entry's wide form first;
- `reg`: `QUERY` (`/V /VE /S /F /K /D /C /E /T /SE`), `ADD` (`/V /VE /T /S /D /F`), `DELETE` (`/V /VE /VA /F`), `COPY`, `EXPORT` and
  `IMPORT` of `.reg` files, and `/reg:32` and `/reg:64`.
