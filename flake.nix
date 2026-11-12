{
  description = "BlastIRC native Wayland IRCv3 desktop client";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      # Match the headers required by the pinned upstream lib/meson.build.
      stbHeaders = pkgs.stb.overrideAttrs (_: {
        version = "unstable-2025-10-26";
        src = pkgs.fetchFromGitHub {
          owner = "nothings";
          repo = "stb";
          rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
          hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
        };
      });
      completionWordlist = (pkgs.scowl.override { singleWordlist = "en-us 60"; }).overrideAttrs (old: {
        # The single-wordlist install phase omits the upstream postInstall hook.
        installPhase = old.installPhase + "\nrunHook postInstall\n";
        postInstall = (old.postInstall or "") + ''
          ${pkgs.glibc.bin}/bin/iconv -f ISO-8859-1 -t UTF-8 \
            "$out/share/dict/words.txt" -o "$out/share/dict/words.utf8.txt"
          mv "$out/share/dict/words.utf8.txt" "$out/share/dict/words.txt"
          install -Dm644 "$src/scowl/Copyright" "$out/share/licenses/scowl/Copyright"
          printf '%s\n' 'SCOWL 2020.12.07, en-us size 60; wordlist converted from ISO-8859-1 to UTF-8 by BlastIRC.' \
            > "$out/share/licenses/scowl/NOTICE"
        '';
      });
      dependencies =
        (with pkgs; [
          sdl3
          sdl3-mixer
          openssl
          curl
          sqlite
          tomlplusplus
          wayland
          wayland-protocols
          libglvnd
          freetype
          fontconfig
          cairo
          pango
          harfbuzz
          libxkbcommon
          glib
          nlohmann_json
          librsvg
          libwebp
          libjxl
          libheif
          libxml2
          mpv-unwrapped
          hunspell
          hunspellDicts.en_US
        ])
        ++ [ stbHeaders ];
      nativeTools = with pkgs; [
        meson
        ninja
        pkg-config
        wayland-scanner
      ];
    in
    {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "blastirc";
        version = "0.1.0";
        meta.license = pkgs.lib.licenses.mit;
        src = pkgs.lib.cleanSourceWith {
          src = self;
          filter =
            path: type:
            let
              relative = pkgs.lib.removePrefix (toString self + "/") (toString path);
            in
            !(builtins.elem (builtins.baseNameOf path) [
              "result"
              ".direnv"
              ".git"
              ".cache"
              ".env"
              "__pycache__"
            ])
            && !(type == "directory" && pkgs.lib.hasPrefix "build" relative)
            && relative != "output"
            && relative != "profiles"
            && relative != "subprojects/noctalia-ui"
            && !(type == "directory" && pkgs.lib.hasPrefix "subprojects/noctalia-" relative)
            && relative != "subprojects/packagecache"
            && relative != "subprojects/.wraplock";
        };
        nativeBuildInputs =
          nativeTools
          ++ (with pkgs; [
            makeWrapper
            autoAddDriverRunpath
          ]);
        buildInputs = dependencies;
        mesonBuildType = "release";
        mesonFlags = [
          "-Dtests=true"
          "-Dfont_dir=${pkgs.nerd-fonts.jetbrains-mono}/share/fonts"
          "-Demoji_dir=${pkgs.noto-fonts-color-emoji}/share/fonts"
          "-Ddictionary_dir=${pkgs.hunspellDicts.en_US}/share/hunspell"
          "-Dwordlist_file=${completionWordlist}/share/dict/words.txt"
        ];
        doCheck = true;
        preCheck = ''
          export XDG_CACHE_HOME="$TMPDIR/blastirc-test-cache"
          export XDG_CONFIG_HOME="$TMPDIR/blastirc-test-config"
          export XDG_DATA_HOME="$TMPDIR/blastirc-test-data"
          mkdir -m 700 -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
          export FONTCONFIG_FILE="${
            pkgs.makeFontsConf {
              includes = [ "${pkgs.fontconfig.out}/etc/fonts/conf.d" ];
              impureFontDirectories = [ ];
              fontDirectories = [
                "${pkgs.dejavu_fonts}/share/fonts"
                "${pkgs.nerd-fonts.jetbrains-mono}/share/fonts"
              ];
            }
          }"
        '';
        postInstall = ''
          install -Dm644 ${pkgs.hunspellDicts.en_US}/share/doc/hunspell-dict-en-us-wordlist.txt \
            $out/share/licenses/blastirc/hunspell-en_US-README.txt
          install -Dm644 ${completionWordlist}/share/licenses/scowl/Copyright \
            $out/share/licenses/blastirc/scowl-Copyright
          install -Dm644 ${completionWordlist}/share/licenses/scowl/NOTICE \
            $out/share/licenses/blastirc/scowl-NOTICE
        '';
        postFixup = ''
          wrapProgram $out/bin/blastirc \
            --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.libnotify ]} \
            --prefix LD_LIBRARY_PATH : ${pkgs.lib.makeLibraryPath [ pkgs.libheif ]}
          substituteInPlace $out/share/blastirc/noctalia-template/apply.sh \
            --replace-fail 'pkill -USR1' '${pkgs.procps}/bin/pkill -USR1' \
            --replace-fail '$(id -u)' '$(${pkgs.coreutils}/bin/id -u)'
          substituteInPlace $out/share/blastirc/noctalia-template/undo.sh \
            --replace-fail 'rm -f' '${pkgs.coreutils}/bin/rm -f' \
            --replace-fail 'dirname --' '${pkgs.coreutils}/bin/dirname --'
        '';
      };
      devShells.${system} = {
        icon-studio = pkgs.mkShellNoCC {
          packages = [ (pkgs.python3.withPackages (ps: [ ps.pillow ])) ];
        };
        default = pkgs.mkShell {
          packages =
            dependencies
            ++ nativeTools
            ++ (with pkgs; [
              gcc
              gdb
              clang-tools
              nixfmt
              desktop-file-utils
              soju
              znc
              ergochat
              (python3.withPackages (ps: [ ps.pillow ]))
              imagemagick
              tesseract
              libnotify
              procps
              nodejs
              ffmpeg
              sway
              wtype
              wl-clipboard
              wlrctl
              grim
              dbus
              xdg-desktop-portal
              xdg-desktop-portal-gtk
              nerd-fonts.jetbrains-mono
              noto-fonts-color-emoji
            ]);
          BLASTIRC_FONT_DIR = "${pkgs.nerd-fonts.jetbrains-mono}/share/fonts";
          BLASTIRC_EMOJI_DIR = "${pkgs.noto-fonts-color-emoji}/share/fonts";
          BLASTIRC_SPELL_DICT_DIR = "${pkgs.hunspellDicts.en_US}/share/hunspell";
          BLASTIRC_WORDLIST_FILE = "${completionWordlist}/share/dict/words.txt";
          BLASTIRC_PORTAL = "${pkgs.xdg-desktop-portal}/libexec/xdg-desktop-portal";
          BLASTIRC_PORTAL_BACKEND = "${pkgs.xdg-desktop-portal-gtk}/libexec/xdg-desktop-portal-gtk";
          BLASTIRC_PORTAL_BACKEND_DIR = "${pkgs.xdg-desktop-portal-gtk}/share/xdg-desktop-portal/portals";
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [ pkgs.libheif ];
          shellHook = ''
            printf '%s\n' \
              'Configure: meson setup build-native --wrap-mode=nodownload -Dfont_dir="$BLASTIRC_FONT_DIR" -Demoji_dir="$BLASTIRC_EMOJI_DIR" -Ddictionary_dir="$BLASTIRC_SPELL_DICT_DIR" -Dwordlist_file="$BLASTIRC_WORDLIST_FILE"' \
              'Build: meson compile -C build-native' \
              'Checks: meson test -C build-native --print-errorlogs' \
              'Package: nix build path:.#default'
          '';
        };
      };
    };
}
