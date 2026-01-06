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
    finalAttrs: prev: {
      pname = "technorino";
      version = "unstable";

      buildInputs = prev.buildInputs ++ [
        pkgs.hunspell
      ];

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

      cmakeFlags = prev.cmakeFlags ++ [
        (pkgs.lib.cmakeBool "CHATTERINO_SPELLCHECK" true)
      ];

      postInstall = ''
        echo "nightly" > $out/bin/modes
      '';
    }
  )
