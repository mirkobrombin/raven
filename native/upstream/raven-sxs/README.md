# Guest sxs component

Raven's own sxs.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64 on
the public Windows headers. `CreateAssemblyNameObject` parses and prints assembly identities, and
`CreateAssemblyCache` installs, queries and removes assemblies and publisher policies under
`C:\Windows\WinSxS`, in the layout the runtime's activation contexts search.

Manifests are read through msxml3. `SxsLookupClrGuid` finds nothing, because the runtime's activation
contexts hold no sections for managed classes. Assembly cache items and scavengers return `E_NOTIMPL`.
See [LICENSES.md](../../../LICENSES.md).
