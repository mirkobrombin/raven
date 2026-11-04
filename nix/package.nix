{
  python311,
  stdenv,
  version,
  src,
  cmake,
  pkgsCross,
  lib,
}:
let
  python = python311;
in
# TODO: other steps for desktop, etc.
stdenv.mkDerivation (finalAttrs: {
  pname = "raven";
  inherit version src;

  nativeBuildInputs = [
    cmake
    python
  ];
  buildInputs = [
    pkgsCross.mingwW64.buildPackages.gcc
    pkgsCross.mingw32.buildPackages.gcc
  ];

  # FIXME: this downloads stuff
  buildPhase = ''
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j2
  '';

  installPhase = ''
    install -Dm755 build/native/raven $out/bin
  '';

  meta = {
    description = "TODO";
    homepage = "useraven.org";
    # FIXME: pretty sure licensing is far more complex than this
    license = lib.licenses.mit;
    maintainers = with lib.maintainers; [ justdeeevin ];
    platforms = lib.platforms.linux;
    mainProgram = "raven";
  };
})
