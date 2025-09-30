{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "48a390d62bf1c7d6815976342530c6a9df13206f";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-ix/Ba4Qxjh/7lDcznE4t3rdzuJoDsAoW4S4gnvZEbCo=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
