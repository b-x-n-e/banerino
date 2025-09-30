{
  description = "Technorino";

  inputs = {
    self.submodules = true;
    nixpkgs.url = "github:NixOS/nixpkgs?rev=bcffac3ac504dc0ad80070251a9a5aae2d4ab339";
  };

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      mkPackages = nixpkgs.lib.genAttrs supportedSystems (system: {
        default = import ./. { inherit (nixpkgs.legacyPackages.${system}) pkgs; };
        package = import ./package.nix { inherit (nixpkgs.legacyPackages.${system}) pkgs; };
      });

      mkShells = nixpkgs.lib.genAttrs supportedSystems (system: {
        default = import ./shell.nix { inherit (nixpkgs.legacyPackages.${system}) pkgs; };
      });

    in
    {
      packages = mkPackages;
      devShells = mkShells;
    };
}
