# Guest structured storage component

Raven's own structured storage for guest programs, built by
`scripts/provision-installer.py` with MinGW-w64 on the public Windows headers. It holds the
compound files that Windows Installer packages and Office documents are made of, following
the public [MS-CFB] specification, the property sets of [MS-OLEPS], streams and lock bytes
on global memory, bind contexts and the class and user type helpers. The runtime's ole32
hands those exports to it.

An open file is a tree of elements that each carry their whole sector chain as an array. The
allocation tables are read to fill those arrays and written anew at every flush, and a
transaction keeps a copy of the tree while sectors it can still see are copied on write.

`exports.txt` lists what the component exports. See [LICENSES.md](../../../LICENSES.md).
