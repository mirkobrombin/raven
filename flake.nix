{
  description = "Nix derivation and devshell for the Raven Windows compatibility layer";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, self, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      packages.${system} = rec {
        raven = pkgs.callPackage ./nix/package.nix {
          version = self.shortRev or self.dirtyShortRev;
          src = ./.;
        };
        default = raven;
      };
    };
}
