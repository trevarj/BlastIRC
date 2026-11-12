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
      wordfreqSource = pkgs.python3Packages.wordfreq.src;
      wordfreqDataLicense = pkgs.fetchurl {
        url = "https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt";
        hash = "sha256-KKlSnH0LtNxR9L9cEWo9Fu8kegUvdZFGZ2jd9WP9HPU=";
      };
      completionWordlist =
        pkgs.runCommand "blastirc-wordfreq-en-small"
          {
            nativeBuildInputs = [ (pkgs.python3.withPackages (ps: [ ps.msgpack ])) ];
            meta.license = pkgs.lib.licenses.cc-by-sa-40;
          }
          ''
            mkdir -p "$out/share/dict" "$out/share/licenses/wordfreq"
            python - "${wordfreqSource}/wordfreq/data/small_en.msgpack.gz" \
              "$out/share/dict/wordfreq-en-small.tsv" <<'PY'
            import gzip
            import sys
            import msgpack

            with gzip.open(sys.argv[1], "rb") as source:
                data = msgpack.load(source, raw=False)
            if not isinstance(data, list) or not data or data[0] != {"format": "cB", "version": 1}:
                raise ValueError("Unexpected wordfreq cBpack header")
            with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as output:
                for frequency_bin, words in enumerate(data[1:]):
                    if not isinstance(words, list):
                        raise ValueError("Unexpected wordfreq frequency bucket")
                    for word in words:
                        if not isinstance(word, str) or not word or any(c in word for c in "\t\r\n\0"):
                            raise ValueError("Word cannot be represented in frequency TSV")
                        output.write(f"{word}\t{frequency_bin}\n")
            PY
            install -m644 "${wordfreqSource}/NOTICE.md" "$out/share/licenses/wordfreq/NOTICE.md"
            install -m644 "${wordfreqSource}/LICENSE.txt" "$out/share/licenses/wordfreq/LICENSE.txt"
            install -m644 "${wordfreqDataLicense}" "$out/share/licenses/wordfreq/CC-BY-SA-4.0.txt"
            cat > "$out/share/licenses/wordfreq/BLASTIRC-CONVERSION.txt" <<'NOTICE'
            BlastIRC English small frequency prior
            Source: wordfreq 3.2.0 by Robyn Speer, copyright 2022 Robyn Speer.
            https://github.com/rspeer/wordfreq/tree/912caf64b657478d1dff1138efdc078947d54bb1
            Data attribution includes Marc Brysbaert et al., creators of the freely
            available SUBTLEX lists; see the complete upstream NOTICE.md for all sources.
            Conversion by BlastIRC: gzip-decompress wordfreq/data/small_en.msgpack.gz,
            decode MessagePack, validate the cBpack format=cB/version=1 header, and
            emit every token in original bucket/token order as UTF-8 token TAB bin LF.
            Bin is the zero-based bucket index after the header; probability is
            10^(-bin/100). No tokens are filtered or normalized during conversion.
            The converted dataset, including any adaptation, is licensed CC BY-SA 4.0:
            https://creativecommons.org/licenses/by-sa/4.0/
            See CC-BY-SA-4.0.txt. The upstream LICENSE.txt applies to upstream code only.
            BlastIRC application code remains MIT; the data is a separate aggregate.
            NOTICE
          '';
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
        meta.license = with pkgs.lib.licenses; [
          mit
          cc-by-sa-40
        ];
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
          "-Dwordlist_file=${completionWordlist}/share/dict/wordfreq-en-small.tsv"
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
          mkdir -p "$out/share/licenses/blastirc"
          cp -r ${completionWordlist}/share/licenses/wordfreq "$out/share/licenses/blastirc/wordfreq"
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
          BLASTIRC_WORDLIST_FILE = "${completionWordlist}/share/dict/wordfreq-en-small.tsv";
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
