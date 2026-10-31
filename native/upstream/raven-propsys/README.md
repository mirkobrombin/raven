# Guest propsys component

Raven's own propsys.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers. It converts, compares and formats `PROPVARIANT` and `VARIANT` values,
provides the in-memory property store with its cache and serialized forms, and answers for the system
properties.

The property table is generated from the keys in the public `propkey.h`. That header names no types,
so `make_schema.py` derives them from the naming the schema uses, and defaults to text. Descriptions
report that name and type and plain defaults for the rest: there are no localized labels, and
registering a schema file is accepted without reading it.

Serialization carries scalars, text, classes and file times. `PSCreatePropertyStoreFromObject` needs
the object to offer `IPropertyStore`. Exports of the public list that are not implemented stop the
caller with an exception. See [LICENSES.md](../../../LICENSES.md).
