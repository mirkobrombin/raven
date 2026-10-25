# Guest RTL component

`scripts/provision-rtl.py` builds selected unchanged ReactOS RTL/CRT files at
`dacf2a6c47fa562799f9c8a41ab6d5c558556558`. The source archive SHA-256 is
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`.
User-mode support functions, Unicode initialization and the verifier notification
retain their original bytes and notices. The build records their source ranges and
hashes, generates official NT/IDL headers, and uses GCC's AMD64 SEH support.

`initialize.c` connects the private RTL initializers to Raven-owned heap-list
storage. The public creation export checks the list bound and unsupported modes
before entering the unchanged allocator. The component imports only NT services
and exception/diagnostic boundaries; it has no DllMain, TLS or upstream loader.
Unimplemented thread/keyed-event/performance-counter/diagnostic paths stop
explicitly. Guest exception handler dispatch remains incomplete.

Upstream COPYING, COPYING.LIB and COPYING.ARM remain beside the extracted sources.
This is a mixed-license component, not an LGPL-only library. `critical.c`,
`heap.c`, `heapuser.c`, `heapdbg.c`, `heappage.c`, `mem.c`, `slist.c`,
`splaytree.c`, `libsupp.c` and `unicode.c` refer to the root GPL terms.
Distribution uses GPL version 2 for the root-COPYING inputs and the version 2
option of the explicitly GPL-2.0-or-later `verifier.c`. Raven-owned
`initialize.c` and `kernel.c` remain MIT; the component is a PE DLL in the guest,
not a native library in the Linux host. `error.c` carries LGPL-2.1-or-later;
`appverifier.c` and `avltable.c` carry the BSD terms in COPYING.ARM. `bitmap.c`
and the `bitmap64.c` instantiation retain their GPL/BSD notice.

`provenance.json` records the compiled files and selected functions. Keep the
complete pinned source archive and the build recipe with `raven-rtl.dll`;
retaining hashes alone is not a source distribution. See
[LICENSES.md](../../../LICENSES.md) and [packaging](../../../docs/packaging.md).
