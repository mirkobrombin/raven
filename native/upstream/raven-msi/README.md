# Guest Windows Installer

Raven's own msi.dll and msiexec.exe for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It reads and writes the package database and its SQL, then runs
the sequences: conditions, formatted text, directories, features and components, files from cabinets and
source folders, registry, shortcuts, services, class registration and product registration where Windows
keeps it. Changes are scheduled, run at `InstallFinalize` and undone on a failure.

Library custom actions run in an `msiexec` host of the library's own width, talking to the installer over
a pipe. Products installed by Click-to-Run answer the queries from their package's manifests.

Absent: the internal dialogs (every level installs without them), patches, advertising, script custom
actions, assemblies in the global cache and rollback or commit custom actions. Those calls answer with the
documented status for the situation. See [LICENSES.md](../../../LICENSES.md).
