#!/usr/bin/env bash
# Install the Fedora packages one stage of the pipeline needs.
#
#   build     compilers, cross compilers and the libraries the README lists
#   test      the build set plus the compositors, input tools and software Vulkan the display tests drive
#   policy    only what the policy and compliance checks run
#
# usage: install-fedora.sh build|test|policy
set -euo pipefail

policy=(git python3)
build=(
    "${policy[@]}" cmake make ccache gcc-c++ clang lld bison patch tar xz zstd openssl
    wine-devel mingw64-gcc mingw64-gcc-c++ mingw32-gcc mingw32-gcc-c++ mingw64-winpthreads-static mingw32-winpthreads-static
    wayland-devel libxkbcommon-devel libxkbcommon-x11-devel libxcb-devel pixman-devel freetype-devel
    vulkan-loader-devel vulkan-headers openssl-devel
    # The configure step registers the software Vulkan tests only when it finds Mesa's driver manifest.
    mesa-vulkan-drivers
    # The text component derives the Raven Sans dialog faces from DejaVu with fontTools.
    python3-fonttools
)
# Display tests start their own headless labwc or Xvfb when these tools are found, and skip themselves otherwise. Text
# tests ask for Arial, Times New Roman and Courier New, which Raven serves from the Liberation and DejaVu faces it finds
# under /usr/share/fonts.
test=(
    "${build[@]}" labwc grim wlrctl wtype wl-clipboard wlr-randr xorg-x11-server-Xvfb openbox xdotool xclip xkeyboard-config
    mesa-dri-drivers python3-pillow mono-devel msitools
    liberation-sans-fonts liberation-serif-fonts liberation-mono-fonts dejavu-sans-fonts dejavu-serif-fonts
    dejavu-sans-mono-fonts
)

case "${1:-}" in
    policy) packages=("${policy[@]}") ;;
    build) packages=("${build[@]}") ;;
    test) packages=("${test[@]}") ;;
    *) echo "usage: $0 build|test|policy" >&2; exit 2 ;;
esac

dnf install -y --setopt=install_weak_deps=False "${packages[@]}"
dnf clean all
