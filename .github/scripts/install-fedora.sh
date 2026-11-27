#!/usr/bin/env bash
# Install the Fedora packages one stage of the pipeline needs.
#
#   build     compilers, cross compilers and the libraries the README lists, and the tools the display tests drive, as the configure
#             step registers each of those tests only when it finds what it needs
#   test      the build set plus the drivers and fonts the tests run with
#   policy    only what the policy and compliance checks run
#
# usage: install-fedora.sh build|test|policy
set -euo pipefail

policy=(git python3)
build=(
    "${policy[@]}" cmake make ccache gcc-c++ clang lld bison patch tar xz zstd openssl cargo rust
    wine-devel mingw64-gcc mingw64-gcc-c++ mingw32-gcc mingw32-gcc-c++ mingw64-winpthreads-static mingw32-winpthreads-static
    wayland-devel wayland-protocols-devel libxkbcommon-devel libxkbcommon-x11-devel libxcb-devel pixman-devel freetype-devel
    # The Wayland desktop is generated from wayland-protocols, and the OpenGL bridge links EGL, GL and GLU.
    mesa-libEGL-devel mesa-libGL-devel mesa-libGLU-devel
    vulkan-loader-devel vulkan-headers openssl-devel
    # The configure step registers the software Vulkan tests only when it finds Mesa's driver manifest, and the ones that run under
    # the validation layer only when it finds the layer.
    mesa-vulkan-drivers vulkan-validation-layers
    # The text component derives the Raven Sans dialog faces from DejaVu with fontTools.
    python3-fonttools
    # The configure step registers the Windows Installer, COM and imaging tests only when it finds wixl to make their packages.
    msitools
    # Display tests start their own headless labwc or Xvfb, and the configure step registers them only when it finds these tools.
    labwc grim wlrctl wtype wl-clipboard wlr-randr xorg-x11-server-Xvfb openbox xdotool xclip xkeyboard-config python3-pillow
    # The .NET workflow builds its WinForms window with Mono's mcs, which the configure step looks for.
    mono-devel
)
# Text tests ask for Arial, Times New Roman and Courier New, which Raven serves from the Liberation and DejaVu faces it finds
# under /usr/share/fonts.
test=(
    "${build[@]}" mesa-dri-drivers
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
