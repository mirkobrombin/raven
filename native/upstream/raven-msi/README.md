# Guest Windows Installer

Raven's own msi.dll and msiexec.exe for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It reads and writes the package database and its SQL, then runs
the sequences: conditions, formatted text, directories, features and components, files from cabinets and
source folders, registry, shortcuts, services, class registration and product registration where Windows
keeps it. Changes are scheduled, run at `InstallFinalize` and undone on a failure, along with the package's
rollback custom actions; its commit custom actions run once the installation has succeeded.

Library custom actions run in an `msiexec` host of the library's own width, talking to the installer over
a pipe. VBScript custom actions run in the installer's own interpreter, which knows the language's statements
and common functions and the session, installer and record objects; it creates no COM objects and has no
classes. .NET assemblies go into the global cache through the fusion of the newest framework installed,
which checks their strong names, and Win32 ones into the side-by-side store. Products installed by
Click-to-Run answer the queries from their package's manifests.

Absent: the internal dialogs (every level installs without them), patches, advertising and JScript custom
actions. Those calls answer with the documented status for the situation. See [LICENSES.md](../../../LICENSES.md).
