# Guest scripting runtime

Raven's own scrrun.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers. It makes the scripting runtime's classes, `Scripting.Dictionary` and
`Scripting.FileSystemObject`, through COM. Scripts reach their objects through `IDispatch`.

- `object.c`, `object.h`: the shared pieces: the `IDispatch` of every object, arguments and
  results, the enumerator of collections, errors as the script languages number them, the class
  factories. The script host object model is built on them too.
- `main.c`: the classes made through COM and the exports.
- `dictionary.c`: `Dictionary`, items by key in the order they were added, found through a hash.
- `fso.c`: `FileSystemObject`: paths, copying, moving and deleting files and folders (with
  wildcards in the last part of a path), text files, special folders and standard streams.
- `objects.c`: `File`, `Folder` and `Drive` with their collections.
- `textstream.c`: `TextStream`, ANSI or UTF-16 text read in pieces or written.

Not yet there: the early-bound interfaces (`IDictionary`, `IFileSystem3` and the rest as dual
interfaces with type information), `Scripting.Encoder` and the `Type` names the shell would give.

`exports.txt` lists the exports. See [LICENSES.md](../../../LICENSES.md).
