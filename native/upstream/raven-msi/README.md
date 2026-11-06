# Guest Windows Installer

Raven's own msi.dll and msiexec.exe for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It reads and writes the package database and its SQL, then runs
the sequences: conditions, formatted text, directories, features and components, files from cabinets and
source folders, registry, shortcuts, services, class registration and product registration where Windows
keeps it. Changes are scheduled, run at `InstallFinalize` and undone on a failure, along with the package's
rollback custom actions; its commit custom actions run once the installation has succeeded.

Library custom actions run in an `msiexec` host of the library's own width, talking to the installer over
a pipe. Script custom actions run in the installer: it hosts the language's Active Scripting engine (the
runtime's own vbscript.dll or jscript.dll) and hands it the session, installer
and record objects. .NET assemblies go into the global cache through the fusion of the newest framework installed,
which checks their strong names, and Win32 ones into the side-by-side store. Products installed by
Click-to-Run answer the queries from their package's manifests.

At the full and reduced user interface levels, which msiexec uses without /q, the package's own dialogs
run the user interface sequence: the Dialog, Control, ControlEvent, ControlCondition and EventMapping
tables give the wizard, its text styles, bitmaps, edits, check boxes, radio buttons, lists and license
text, and the progress dialog follows the installation. Directory lists, feature trees, volume lists and
billboards are not shown yet; a wizard goes on without them.

Absent: patches and advertising. Those calls answer with the documented status for the situation. See [LICENSES.md](../../../LICENSES.md).
