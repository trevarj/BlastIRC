#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f build-native/build.ninja ]]; then
  meson setup build-native --wrap-mode=nodownload \
    -Dfont_dir="$BLASTIRC_FONT_DIR" -Demoji_dir="$BLASTIRC_EMOJI_DIR" \
    -Ddictionary_dir="$BLASTIRC_SPELL_DICT_DIR" -Dwordlist_file="$BLASTIRC_WORDLIST_FILE"
fi
meson compile -C build-native blastirc
if [[ ${1:-} == --build-only ]]; then
  exit 0
fi
exec ./build-native/blastirc "$@"
