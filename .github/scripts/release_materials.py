#!/usr/bin/env python3
"""Write the --materials file scripts/package-native.py bundles a release with.

Each component built from ReactOS keeps the source archives it was built from (ReactOS's, and any other upstream release
such as zlib's), the recipe that built it (provenance.json)
and the license notices it carries. Those are the corresponding source materials of docs/packaging.md; a component without
all three is not listed, and the bundle step then refuses a build that configured it.

usage: release_materials.py COMPONENTS OUTPUT
"""

import hashlib
import json
import pathlib
import sys

KEYS = {
    "rtl": ["RAVEN_RTL_COMPONENT_PATH"],
    "controls": ["RAVEN_CONTROLS_COMPONENT_DIR"],
    "controls32": ["RAVEN_CONTROLS32_COMPONENT_DIR"],
    "installer": ["RAVEN_INSTALLER_COMPONENT_DIR"],
    "installer32": ["RAVEN_INSTALLER32_COMPONENT_DIR"],
}


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            hasher.update(block)
    return hasher.hexdigest()


def main():
    components, output = (pathlib.Path(value).resolve() for value in sys.argv[1:3])
    materials = []
    for name, keys in KEYS.items():
        directory = components / name
        source, recipe = directory / "reactos-source.tar.gz", directory / "provenance.json"
        notices = sorted(path for path in (directory / "licenses").glob("*") if path.is_file())
        if not (source.is_file() and recipe.is_file() and notices):
            print(f"{name}: no source archive, recipe or notices; not listed", file=sys.stderr)
            continue
        materials.append(dict(name=f"reactos-{name}", cmake_keys=keys, source=str(source), sha256=digest(source),
                              recipe=str(recipe), notices=[str(path) for path in notices]))
        # The other upstream archives the component was built from as well, such as zlib's own release.
        for other in sorted(directory.glob("*-source.tar.gz")):
            if other != source:
                materials.append(dict(name=f"{other.name.removesuffix('-source.tar.gz')}-{name}", cmake_keys=keys, source=str(other),
                                      sha256=digest(other), recipe=str(recipe), notices=[str(path) for path in notices]))
    output.write_text(json.dumps(materials, indent=2) + "\n")
    print(f"{len(materials)} components with source materials")
    return 0 if materials else 1


if __name__ == "__main__":
    sys.exit(main())
