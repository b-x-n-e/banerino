{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "7c3e3c4a03cd909c05ab06d5dd0d115d600caa47";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-0/Wsx4PA2K+2lzuZ6wL2XGZe1HRx1o5Hn2KTcBfyw+o=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
