# Guest oleaut32 component

Raven's own oleaut32.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It holds BSTRs and their cache, safe arrays, variants
with their conversions, arithmetic and formatting, record info, the type library reader,
builder and MSFT writer, the dispatch helpers, the standard font and picture objects, connection
points, the marshalling helpers for the automation wire types, and the proxies and stubs of
interfaces a type library describes.

`stdole2.idl` and `resources.rc` produce the standard OLE type library this file carries.

SLTG type libraries are not read, and a library built in memory is always written as MSFT.
Type name hashes only cover ASCII names. Proxies made from a type library answer the IDispatch
methods of a dual interface from the description; plain dispatch interfaces (`PSDispatch`), records
holding pointers and variants passed by reference do not cross. `OleCreatePropertyFrame`, `EnumConnectionPoints`, `DeleteTypeInfo` and `SetMops` return
`E_NOTIMPL`.

`exports.txt` lists the exports: `name` by name, `name=other` under a second name, an `@n` entry
at that ordinal without a name. Entries that point at `oleaut_missing` return `E_NOTIMPL`. See
[LICENSES.md](../../../LICENSES.md).
