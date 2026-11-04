#!/usr/bin/env bash
# The provisioned components the pipeline builds and tests with, in one place.
#
#   unpack    extract the downloaded component archives into $COMPONENTS
#   options   print the CMake options that configure them, one per line
#
# usage: components.sh unpack|options
set -euo pipefail

components=${COMPONENTS:?COMPONENTS names the directory the components live in}
archives=${ARCHIVES:-/opt/ci/archives}

case "${1:-}" in
    unpack)
        mkdir -p "${components}"
        for archive in "${archives}"/*.tar.zst; do
            zstd -d -q -c "${archive}" | tar -C "${components}" -xf -
        done
        ;;
    options)
        cat <<EOF
-DRAVEN_RTL_COMPONENT_PATH=${components}/rtl/raven-rtl.dll
-DRAVEN_CONTROLS_COMPONENT_DIR=${components}/controls
-DRAVEN_CONTROLS32_COMPONENT_DIR=${components}/controls32
-DRAVEN_INSTALLER_COMPONENT_DIR=${components}/installer
-DRAVEN_INSTALLER32_COMPONENT_DIR=${components}/installer32
-DRAVEN_SQLITE_COMPONENT_DIR=${components}/sqlite
-DRAVEN_DXVK_PE64_DIR=${components}/graphics/dxvk/pe64
-DRAVEN_DXVK_PE32_DIR=${components}/graphics/dxvk/pe32
-DRAVEN_VKD3D_PE64_DIR=${components}/graphics/vkd3d-proton/pe64
-DRAVEN_ROOT_CERTIFICATE_DIRECTORY=${components}/roots
-DPNG_LIBRARY_RELEASE=${components}/libpng/install/lib/libpng16.a
-DPNG_PNG_INCLUDE_DIR=${components}/libpng/install/include
-DRAVEN_SKIA_SOURCE=${components}/text/skia
-DRAVEN_SKIA_LIBRARY=${components}/text/cpu-fonts/libskia.a
-DRAVEN_FONT_DIRECTORY=${components}/text/fonts
-DRAVEN_TESTDATA_DIR=${components}/testdata
EOF
        ;;
    *)
        echo "usage: $0 unpack|options" >&2
        exit 2
        ;;
esac
