{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "34230a15dd56c688c3bfe7ca5d31a86a7358f6ca";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-XR+uEkb+UiBLs0wFRFsafVB+896ZrDioiLAAng8RoJ0=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
