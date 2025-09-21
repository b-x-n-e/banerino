{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "1f94fbe99190835e9f8f035797ae0b30229db14d";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-EjyeCVu9gqZXULKaAqaDigdI4WQgj2te9Rp/4NQ/lnA=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
