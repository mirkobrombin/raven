# CP1252 canonical decomposition

cp1252-decomposition.inc selects canonical decompositions from UnicodeData 15.1.0
for the characters in the pinned CP1252 byte table of
`native/upstream/windows-codepages`. Compatibility forms
are excluded. All selected sequences contain two terminal scalars. Provenance
records the complete upstream data hash, selection hash and Unicode license.
The table is used only for MB_COMPOSITE; no Unicode runtime dependency is added.

# Normalization data

normalization.inc holds the expansions, combining classes and primary composites of
Unicode 16.0.0 for `NormalizeString` and `IsNormalizedString`, made by
`scripts/generate-normalization.py` from the same Unicode Character Database.
