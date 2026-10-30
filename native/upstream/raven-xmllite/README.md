# XmlLite

`xmllite.dll`, built for both widths by `scripts/provision-installer.py`: the streaming XML reader and writer.

It is Raven code written from the XML 1.0 and Namespaces in XML specifications and Microsoft's public XmlLite documentation.

- `reader.c`: `IXmlReader`. Reads the whole input on the first `Read` and turns it to UTF-16, by byte order mark, the caller's encoding or the
  document's declaration. A DOCTYPE is refused unless `DtdProcessing_Parse` is set, and then its name is reported and its subset skipped, with
  nothing in it applied: an entity it declares is still undeclared in the content.
- `writer.c`: `IXmlWriter`. Keeps the start tag open for attributes and writes namespace declarations last in it. Writes a byte order mark for
  UTF-16 only, and a declaration only for `WriteStartDocument`.
- `guid.c`: the interface identifiers.
