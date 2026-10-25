# Packaging Raven

A release provides two files:

- `raven-VERSION-linux-x86_64.tar.gz`: executable, guest components and notices.
- `raven-VERSION-source.tar.gz`: the corresponding Raven and third-party source,
  modifications and build/install recipes.

Publish both in the same release with equivalent access. Users need only the
binary archive to run Raven on a supported Linux system. Its `SOURCE.txt` names
the matching source download. Source availability is the distributor's job;
users do not need to classify licenses or collect DLLs.

## Build and test

Use a clean checkout with a build directory outside it. Set SOURCE, BUILD and
ARTIFACTS to absolute paths. Configure the components needed for that release,
using the provisioners documented in the main README.

```sh
cmake -S "$SOURCE" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo
python3 "$SOURCE/scripts/package-native.py" prepare --source "$SOURCE" --build "$BUILD"
cmake --build "$BUILD" -j2
python3 "$SOURCE/scripts/package-native.py" seal --source "$SOURCE" --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure -j1 \
  --output-junit "$BUILD/ctest-results.xml"
python3 "$SOURCE/scripts/package-native.py" bundle \
  --source "$SOURCE" --build "$BUILD" \
  --test-report "$BUILD/ctest-results.xml" \
  --materials /absolute/path/to/materials.json \
  --output "$ARTIFACTS/raven-VERSION-linux-x86_64.tar.gz" \
  --source-output "$ARTIFACTS/raven-VERSION-source.tar.gz"
```

The release command records source inputs before building the host. The final seal, after all
runtime components are built, records their hashes. Changing source, configuration
or runtime files requires a rebuild. Packaging checks the complete configured
CTest report, refuses failures and preserves executable permissions. Skipped
checks are recorded, not treated as application compatibility evidence. Do not
upload an additional raw build directory that bypasses packaging.

## Corresponding source material

The maintainer supplies a JSON inventory of configured third-party inputs:

```json
[
  {
    "name": "reactos-controls-x64",
    "cmake_keys": ["RAVEN_CONTROLS_COMPONENT_DIR"],
    "source": "/absolute/path/to/reactos-source.tar.gz",
    "sha256": "5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69",
    "recipe": "/absolute/path/to/controls/provenance.json",
    "notices": [
      "/absolute/path/to/controls/licenses/COPYING",
      "/absolute/path/to/controls/licenses/COPYING.LIB",
      "/absolute/path/to/controls/licenses/COPYING.ARM"
    ]
  }
]
```

One archive may cover several configuration keys. Include all applicable notices,
not just the GNU texts in this example. The ReactOS source archive, Raven's
provisioning scripts and patches, and the recorded provenance supply the inputs
and modifications. Include generated files too when the recipe cannot recreate
them. The provisioners retain upstream root and module notices in `licenses/`
and record their hashes. Modified source copies carry a dated modification notice.

Static Skia requires its exact source, actual GN arguments and compiled dependency
sources/notices. The libpng recipe and sources must match the linked library.
Configured Liberation fonts are included in `runtime/fonts`, which the executable
finds beside itself. Their copyright and OFL terms are included in `LICENSES/`.
Graphics backends need the matching source and
build configuration, not just downloaded PE binaries. Certificate bundles retain
their own provenance and notices. Unmodified compiler tools and system libraries
need not be copied into this source inventory solely because they build or run
Raven. Added static dependencies or bundled shared libraries need their own review.

The material checks validate hashes and configuration coverage. They cannot infer
the completeness of a manually supplied recipe. Record the full build recipe,
including tool versions and options; an upstream URL or main-branch link alone
is not the corresponding source.

## Verify the archives

Each archive contains SHA256SUMS, BUILD-INFO.json and license notices. Extract it
and run `sha256sum -c SHA256SUMS`. The binary archive contains `runtime/` and
identifies the matching source archive. The source archive contains `raven.tar`
and `sources/` with third-party inputs and recipes. Extract `raven.tar` and follow
the README and recorded recipes to rebuild; the extracted source builds without
Git. A Git checkout is needed only to create a new release record.

Keep the corresponding source accessible alongside the binary download under
GPLv3 section 6(d), and satisfy each guest component's applicable source terms.
The full source/build material allows recipients to modify and rebuild the LGPL
portions too. Do not restrict reverse engineering needed to debug those changes.
CI release jobs must depend on successful tests and upload these archives together.
