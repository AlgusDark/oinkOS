{
  description = "oinkOS development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      in {
        # PlatformIO stays in .venv (see .envrc): the build needs the pinned
        # pioarduino core, which nixpkgs does not package.
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            llvmPackages_latest.clang-tools # clang-format for bin/clang-format-fix
            python3
            uv
          ];
        };
      });
}
