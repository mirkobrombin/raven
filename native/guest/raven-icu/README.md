# raven-icu

ICU's C interface for programs that import it, such as winget, under the unsuffixed names Windows uses in `icu.dll`, with `icuuc.dll` and `icuin.dll` forwarding to it.

This is Raven's own code, written from ICU's public API documentation and from Unicode's UAX #9, #11, #14, #24 and #29. It carries no ICU source or data: case mapping, character types and collation come from the system's text functions.

It covers case mapping and folding, break iteration, UText over UTF-8 and UTF-16, code page conversion, collation, regular expressions, script detection, the bidirectional algorithm and a few transliterators.

Known limits:

- Line and sentence breaking follow the main rules of UAX #14 and #29, not every class.
- East Asian Width is exact for the wide, fullwidth and halfwidth blocks. The other classes read as neutral or narrow.
- Regular expressions have no lookbehind, no set arithmetic and no POSIX classes; those patterns fail with `U_REGEX_UNIMPLEMENTED`. `\p{...}` knows only the main general categories. Possessive loops and atomic groups behave as greedy ones.
- Locale tailoring of case mapping and breaking is not applied.
- Scripts and bidirectional classes come from Unicode blocks: punctuation and symbols inside a script's block read as Common.
- The bidirectional algorithm applies the weak, neutral and implicit rules and reordering; explicit embeddings, overrides, isolates and bracket pairs are not applied, and caller-supplied levels fail with `U_UNSUPPORTED_ERROR`.
- Transliterators exist for Null, Lower, Upper, and Latin-ASCII or `NFD; [:Nonspacing Mark:] Remove; NFC`, which only take accents off. Other IDs fail with `U_INVALID_ID` and rule-based ones with `U_UNSUPPORTED_ERROR`.
