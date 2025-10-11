{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "22a166de9cfb3ca53e2d18b29aac35dcf7b04855";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-FDQ2Nws5yF17Nviizg10nsHYBrwJY4jEABhVFkkMAtY=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
