# raven-icu

ICU's C interface for programs that import it, such as winget, under the unsuffixed names Windows uses in `icu.dll`, with `icuuc.dll` and `icuin.dll` forwarding to it.

This is Raven's own code, written from ICU's public API documentation and from Unicode's UAX #11, #14 and #29. It carries no ICU source or data: case mapping, character types and collation come from the system's text functions.

It covers case mapping and folding, break iteration, UText over UTF-8 and UTF-16, code page conversion, collation and regular expressions.

Known limits:

- Line and sentence breaking follow the main rules of UAX #14 and #29, not every class.
- East Asian Width is exact for the wide, fullwidth and halfwidth blocks. The other classes read as neutral or narrow.
- Regular expressions have no lookbehind, no set arithmetic and no POSIX classes; those patterns fail with `U_REGEX_UNIMPLEMENTED`. `\p{...}` knows only the main general categories. Possessive loops and atomic groups behave as greedy ones.
- Locale tailoring of case mapping and breaking is not applied.
