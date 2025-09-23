{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "0f6e606b996aad13784dab98a9a9ddd3fab0a205";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-ifDR336OWS8udFIxysOMuhzjymyRASdAcTAie5Qef1A=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
