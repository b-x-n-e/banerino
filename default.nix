{
  pkgs ? import <nixpkgs> { },
  gitHash ? "",
}:

let
  chatterino2 = pkgs.chatterino2;
in

(chatterino2.buildChatterino {
  enableAvifSupport = true;
}).overrideAttrs
  (
    finalAttrs: _: {
      pname = "technorino";
      version = "unstable";

      src = pkgs.nix-gitignore.gitignoreSourcePure [
        "*build-*/"
        "result"
        "*-source/"
        ".git"
      ] ./.;

      preConfigure = ''
        export GIT_HASH="${builtins.substring 0 9 gitHash}"
        echo GIT_HASH=$GIT_HASH
      '';
    }
  )
