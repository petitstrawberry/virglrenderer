{
  description = "VirGL renderer with a macOS Metal compute buffer backend";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/3e41b24abd260e8f71dbe2f5737d24122f972158";
  outputs = { self, nixpkgs }:
    let systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
    in {
      packages = nixpkgs.lib.genAttrs systems (system:
        let pkgs = import nixpkgs { inherit system; };
        in { default = pkgs.callPackage ./nix/package.nix { src = self; }; });
      devShells = nixpkgs.lib.genAttrs systems (system:
        let pkgs = import nixpkgs { inherit system; };
        in { default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          packages = [ pkgs.meson pkgs.ninja pkgs.pkg-config
            (pkgs.python3.withPackages (p: [ p.pyyaml ])) ];
        }; });
    };
}
