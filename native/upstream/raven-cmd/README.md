# Command processor

`cmd.c` is Raven code, built by CMake with MinGW into `cmd.exe` (and `x86/cmd.exe`) on the public Windows headers, without a C
runtime. Raven copies it into every prefix as `C:\Windows\System32\cmd.exe` and `C:\Windows\SysWOW64\cmd.exe`, where `ComSpec` points,
and `CreateProcess` runs batch files through it as `cmd /c "..."`.

It is written from Microsoft's public description of the command processor and its commands, and from how they behave; no Windows
code is used or consulted, nor the command processors of other projects.

What it does:

- the options `/C`, `/K`, `/R`, `/Q`, `/S`, `/D`, `/V:ON` and `/T:fg`, with the quote rule `cmd /?` describes for the command after
  `/C` or `/K`;
- a prompt from `PROMPT` (`$P$G` by default, with the codes `PROMPT /?` lists). At a console it has its own line editor: Left, Right,
  Home and End (by words, or deleting to either end, with Ctrl), Backspace, Delete, Insert, Escape, F3, Up and Down through the
  commands typed before, and Tab or Shift+Tab completing file names. `More?` asks for the rest of an open block;
- the phases of a line: `%NAME%` with `:~start,count` and `:old=new` edits, `%ERRORLEVEL%`, `%CD%`, `%DATE%`, `%TIME%`, `%RANDOM%`
  and the other dynamic variables; in batch files `%0` to `%9`, `%*`, `%%` and the `%~fdpnxsatz` and `%~$VAR:` modifiers. Then
  carets, quotes, `&`, `&&`, `||`, `|`, the redirections `<`, `>`, `>>`, `n>` and `n>&m`, and parenthesised blocks over several
  lines; FOR variables and, with delayed expansion, `!NAME!` as each command runs;
- `IF` with `/I`, `NOT`, `ERRORLEVEL`, `EXIST`, `DEFINED`, `CMDEXTVERSION`, `==` and `EQU NEQ LSS LEQ GTR GEQ`, and `ELSE`;
- `FOR` over sets of words and files, and with `/D`, `/R`, `/L` and `/F` (`eol`, `skip`, `delims`, `tokens`, `usebackq`; over
  files, strings and the output of a command);
- batch files with labels, `GOTO`, `CALL` of other batch files and of labels with their own arguments, `SHIFT`, `SETLOCAL` and
  `ENDLOCAL`, `EXIT /B`, and Ctrl+C asking whether to end the batch job;
- the built-ins `ASSOC`, `BREAK`, `CALL`, `CD`/`CHDIR` (with each drive's own folder), `CLS`, `COLOR`, `COPY` (with wildcards and
  `+`), `DATE`, `DEL`/`ERASE`, `DIR` (`/A`, `/B`, `/L`, `/O`, `/S`, `/W`, `/-C`, `DIRCMD`), `ECHO`, `ENDLOCAL`, `EXIT`, `FTYPE`,
  `GOTO`, `IF`, `FOR`, `MD`/`MKDIR`, `MKLINK`, `MOVE`, `PATH`, `PAUSE`, `POPD`, `PROMPT`, `PUSHD`, `RD`/`RMDIR`, `REM`,
  `REN`/`RENAME`, `SET` (with `/A` and `/P`), `SETLOCAL`, `SHIFT`, `START`, `TIME`, `TITLE`, `TYPE`, `VER`, `VERIFY` and `VOL`, each
  with a short `/?`;
- programs found by their path, or in the current folder and then `PATH`, with the extensions of `PATHEXT`; a program's exit code
  becomes `ERRORLEVEL`, and a command nothing runs sets it to 9009.

As on Windows, both sides of a pipe and the command of `FOR /F` run in a command processor of their own, so a variable a side of a
pipe sets does not reach the prompt. Redirections reach handles 3 to 9 (`3>log.txt`, then `1>&3`), for the commands of this command
processor; programs it starts get the standard three.
