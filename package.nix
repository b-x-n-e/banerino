{
  pkgs ? import <nixpkgs> { },
}:

let
  gitHash = "3e52f0ff928c923a1fcc9a3e82c3c6ced6fc76c9";
  repo = pkgs.fetchFromGitHub {
    owner = "2547techno";
    repo = "technorino";
    rev = gitHash;
    hash = "sha256-7xpoyyzx1ScsotwF+Z1ykX83znf+m5Imp9M5i5a/Fps=";
    fetchSubmodules = true;
  };
  technorino = import "${repo}/default.nix" { inherit pkgs gitHash; };
in
technorino
