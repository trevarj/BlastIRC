project('blastirc', 'c', 'cpp',
  version: '0.1.0',
  meson_version: '>=1.3.0',
  default_options: ['cpp_std=c++23', 'warning_level=3', 'buildtype=debugoptimized'],
)

noctalia = subproject('noctalia-ui', default_options: ['library_only=true'])
noctalia_ui_dep = noctalia.get_variable('noctalia_ui_dep')
noctalia_ui_codecs_dep = noctalia.get_variable('noctalia_ui_codecs_dep')
noctalia_ui_assets_dir = noctalia.get_variable('noctalia_ui_assets_dir')
fs = import('fs')
noctalia_source_dir = fs.parent(fs.parent(noctalia_ui_assets_dir))

src_inc = include_directories('src')
openssl_dep = dependency('openssl')
threads_dep = dependency('threads')
curl_dep = dependency('libcurl', version: '>=7.85.0')
webp_dep = dependency('libwebp')
xml_dep = dependency('libxml-2.0')
mpv_dep = dependency('mpv')
hunspell_dep = dependency('hunspell')
pango_dep = dependency('pango')
sqlite_dep = dependency('sqlite3')
toml_dep = dependency('tomlplusplus')
fontconfig_dep = dependency('fontconfig')
sdl_dep = dependency('sdl3')
sdl_mixer_dep = dependency('sdl3-mixer')
gio_dep = dependency('gio-2.0')
json_dep = dependency('nlohmann_json')

irc_lib = static_library('blastirc-irc',
  'src/irc/network.cpp',
  'src/irc/protocol.cpp',
  'src/irc/sts.cpp',
  include_directories: src_inc,
  dependencies: [openssl_dep, threads_dep],
)
irc_dep = declare_dependency(
  link_with: irc_lib,
  include_directories: src_inc,
  dependencies: [openssl_dep, threads_dep],
)
avatar_lib = static_library('blastirc-avatar', 'src/avatar.cpp',
  include_directories: src_inc,
  dependencies: [curl_dep, webp_dep, threads_dep],
)
avatar_dep = declare_dependency(
  link_with: avatar_lib,
  include_directories: src_inc,
  dependencies: [curl_dep, webp_dep, threads_dep],
)
preview_lib = static_library('blastirc-preview', 'src/preview.cpp',
  include_directories: src_inc,
  dependencies: [avatar_dep, curl_dep, webp_dep, xml_dep, threads_dep],
)
preview_dep = declare_dependency(
  link_with: preview_lib,
  include_directories: src_inc,
  dependencies: [avatar_dep, curl_dep, webp_dep, xml_dep, threads_dep],
)
upload_lib = static_library('blastirc-upload', 'src/upload.cpp',
  include_directories: src_inc,
  dependencies: [curl_dep, json_dep, threads_dep],
)
upload_dep = declare_dependency(
  link_with: upload_lib,
  include_directories: src_inc,
  dependencies: [curl_dep, json_dep, threads_dep],
)
picker_lib = static_library('blastirc-file-picker', 'src/platform/file_picker.cpp',
  include_directories: src_inc,
  dependencies: [gio_dep, threads_dep, upload_dep],
)
picker_dep = declare_dependency(
  link_with: picker_lib,
  include_directories: src_inc,
  dependencies: [gio_dep, threads_dep, upload_dep],
)
storage_deps = [sqlite_dep, toml_dep, upload_dep]
asset_dir = get_option('datadir') / 'blastirc' / 'assets'
app_cpp_args = [
  '-DBLASTIRC_ASSET_DIR="' + (get_option('prefix') / asset_dir) + '"',
  '-DBLASTIRC_FONT_DIR="' + get_option('font_dir') + '"',
  '-DBLASTIRC_EMOJI_DIR="' + get_option('emoji_dir') + '"',
  '-DBLASTIRC_SPELL_DICT_DIR="' + get_option('dictionary_dir') + '"',
]
spell_lib = static_library('blastirc-spellcheck', 'src/spellcheck.cpp',
  include_directories: src_inc,
  dependencies: [hunspell_dep, pango_dep, threads_dep],
  cpp_args: app_cpp_args,
)
spell_dep = declare_dependency(
  link_with: spell_lib,
  include_directories: src_inc,
  dependencies: [hunspell_dep, pango_dep, threads_dep],
)

executable('blastirc',
  'src/main.cpp',
  'src/application.cpp',
  'src/storage.cpp',
  'src/platform/window.cpp',
  'src/platform/clipboard.cpp',
  'src/platform/capture.cpp',
  'src/ui/ui.cpp',
  'src/ui/mode_line.cpp',
  'src/ui/buffer_chrome.cpp',
  'src/ui/sidebar.cpp',
  'src/ui/dialogs.cpp',
  'src/ui/workspace.cpp',
  'src/ui/editor.cpp',
  'src/ui/command_completion.cpp',
  'src/ui/emoji.cpp',
  'src/ui/emoji_picker.cpp',
  'src/ui/message_selection.cpp',
  'src/ui/transcript.cpp',
  'src/ui/media_preview.cpp',
  'src/ui/media_player.cpp',
  'src/ui/presentation.cpp',
  'src/render/graphics.cpp',
  'src/theme/theme.cpp',
  'src/fx/effects.cpp',
  include_directories: src_inc,
  dependencies: [irc_dep, avatar_dep, preview_dep, picker_dep, spell_dep, noctalia_ui_dep, noctalia_ui_codecs_dep,
    mpv_dep, sdl_dep, sdl_mixer_dep, fontconfig_dep] + storage_deps,
  cpp_args: app_cpp_args,
  install: true,
)

if get_option('tests')
  check_args = ['-UNDEBUG']
  protocol_check = executable('blastirc-protocol-check', 'src/irc/core_check.cpp',
    dependencies: irc_dep, cpp_args: check_args)
  test('protocol', protocol_check)

  network_check = executable('blastirc-network-check', 'tests/network_check.cpp',
    dependencies: [irc_dep, avatar_dep], cpp_args: check_args)
  test('network', network_check)
  test('soju-wire', network_check, args: ['--soju'])

  avatar_check = executable('blastirc-avatar-check', 'tests/avatar_check.cpp',
    dependencies: avatar_dep, cpp_args: check_args)
  test('avatar', avatar_check)

  preview_check = executable('blastirc-preview-check', 'tests/preview_check.cpp',
    dependencies: [preview_dep, openssl_dep], cpp_args: check_args)
  test('preview', preview_check)

  spellcheck_check = executable('blastirc-spellcheck-check', 'tests/spellcheck_check.cpp',
    dependencies: spell_dep, cpp_args: check_args + app_cpp_args)
  test('spellcheck', spellcheck_check)

  media_player_check = executable('blastirc-media-player-check',
    'tests/media_player_check.cpp', 'src/ui/media_player.cpp',
    dependencies: [preview_dep, mpv_dep], cpp_args: check_args)
  test('media_player', media_player_check)

  upload_check = executable('blastirc-upload-check', 'tests/upload_check.cpp',
    dependencies: [upload_dep, openssl_dep], cpp_args: check_args)
  test('upload', upload_check)

  ui_check = executable('blastirc-ui-check', 'src/ui/logic_check.cpp',
    include_directories: src_inc, cpp_args: check_args)
  test('workspace_logic', ui_check)

  workspace_check = executable('blastirc-workspace-check',
    'tests/workspace_check.cpp', 'src/ui/workspace.cpp',
    include_directories: src_inc, cpp_args: check_args)
  test('workspace_check', workspace_check)

  native_input_check = executable('blastirc-native-input-check', 'tests/native_input_check.cpp',
    include_directories: src_inc, dependencies: noctalia_ui_dep, cpp_args: check_args)
  test('native_input_check', native_input_check)

  clipboard_check = executable('blastirc-clipboard-check', 'tests/clipboard_check.cpp',
    include_directories: src_inc, dependencies: noctalia_ui_dep, cpp_args: check_args)
  test('clipboard_check', clipboard_check)

  native_style_check = executable('blastirc-native-style-check', 'tests/native_style_check.cpp',
    include_directories: src_inc, dependencies: noctalia_ui_dep, cpp_args: check_args)
  test('native_style_check', native_style_check)

  cursor_check = executable('blastirc-cursor-check', 'tests/cursor_check.cpp',
    include_directories: src_inc, dependencies: noctalia_ui_dep, cpp_args: check_args)
  test('cursor_check', cursor_check)

  native_theme_check = executable('blastirc-native-theme-check',
    'tests/native_theme_check.cpp', 'src/theme/theme.cpp', 'src/ui/presentation.cpp',
    include_directories: src_inc, dependencies: [noctalia_ui_dep, toml_dep],
    cpp_args: check_args)
  test('native_theme_check', native_theme_check)

  native_ui_check = executable('blastirc-native-ui-check',
    'tests/native_rename_check.cpp', 'tests/composer_check.cpp', 'tests/emoji_check.cpp',
    'tests/settings_check.cpp', 'tests/transcript_check.cpp',
    'tests/ui_integration_check.cpp',
    'tests/media_preview_check.cpp',
    'src/application.cpp', 'src/storage.cpp',
    'src/platform/clipboard.cpp',
    'src/ui/ui.cpp', 'src/ui/mode_line.cpp', 'src/ui/buffer_chrome.cpp',
    'src/ui/sidebar.cpp', 'src/ui/dialogs.cpp', 'src/ui/workspace.cpp',
    'src/ui/editor.cpp', 'src/ui/emoji.cpp', 'src/ui/emoji_picker.cpp',
    'src/ui/command_completion.cpp',
    'src/ui/message_selection.cpp', 'src/ui/transcript.cpp', 'src/ui/presentation.cpp',
    'src/ui/media_preview.cpp', 'src/ui/media_player.cpp',
    'src/render/graphics.cpp', 'src/theme/theme.cpp', 'src/fx/effects.cpp',
    include_directories: src_inc,
    dependencies: [irc_dep, avatar_dep, preview_dep, picker_dep, spell_dep, noctalia_ui_dep, noctalia_ui_codecs_dep,
      mpv_dep, openssl_dep, sdl_dep, sdl_mixer_dep, fontconfig_dep] + storage_deps,
    cpp_args: check_args + app_cpp_args)
  test('native_rename_check', native_ui_check, args: ['rename'])
  test('native_composer_check', native_ui_check, args: ['composer'])
  test('native_settings_check', native_ui_check, args: ['settings'])
  test('native_transcript_check', native_ui_check, args: ['transcript'])
  test('native_shell_check', native_ui_check, args: ['shell'])
  test('native_media_preview_check', native_ui_check, args: ['media-preview'],
    env: ['ALSOFT_DRIVERS=null'])

  storage_check = executable('blastirc-storage-check',
    'tests/storage_check.cpp', 'src/storage.cpp',
    include_directories: src_inc, dependencies: [irc_dep] + storage_deps, cpp_args: check_args)
  test('storage', storage_check)

  fx_check = executable('blastirc-fx-check', 'tests/fx_check.cpp', cpp_args: check_args)
  test('effects_envelopes', fx_check)

  fx_integration_check = executable('blastirc-fx-integration-check',
    'tests/fx_check.cpp', 'src/render/graphics.cpp', 'src/theme/theme.cpp',
    'src/ui/presentation.cpp',
    include_directories: src_inc,
    dependencies: [noctalia_ui_dep, noctalia_ui_codecs_dep, sdl_dep, sdl_mixer_dep,
      fontconfig_dep, toml_dep],
    cpp_args: check_args + app_cpp_args + ['-DBLASTIRC_FX_INTEGRATION'])
  test('effects_headless_integration', fx_integration_check, workdir: meson.project_source_root())

  application_check = executable('blastirc-application-check',
    'tests/application_regression.cpp', 'src/application.cpp', 'src/storage.cpp',
    include_directories: src_inc, dependencies: [irc_dep, avatar_dep] + storage_deps,
    cpp_args: check_args)
  test('application', application_check)

  executable('blastirc-network-probe', 'tests/integration/network_probe.cpp',
    include_directories: src_inc, dependencies: irc_dep, cpp_args: check_args)
endif

# The SDK requires the real English catalog before accepting an application bundle.
translations_dir = noctalia_source_dir / 'assets' / 'translations'
install_subdir(translations_dir, install_dir: asset_dir)
install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: asset_dir)
install_data(noctalia.get_variable('noctalia_ui_emoji'), install_dir: asset_dir)
install_subdir('assets/lucide', install_dir: asset_dir)
install_subdir('assets/sfx', install_dir: asset_dir)
install_data('assets/icon.png', install_dir: asset_dir)
install_data('assets/icon.png',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / '512x512' / 'apps',
  rename: 'io.github.trevarj.blastirc.png')
install_subdir('noctalia-template', install_dir: get_option('datadir') / 'blastirc')
install_data('packaging/io.github.trevarj.blastirc.desktop',
  install_dir: get_option('datadir') / 'applications')

license_dir = get_option('datadir') / 'licenses' / 'blastirc'
install_data('LICENSE', install_dir: license_dir)
install_data('assets/lucide/LICENSE', install_dir: license_dir, rename: 'lucide-LICENSE')
install_data('assets/sfx/LICENSE', install_dir: license_dir, rename: 'sfx-LICENSE')
install_data(noctalia.get_variable('noctalia_ui_license'),
  install_dir: license_dir, rename: 'noctalia-ui-LICENSE.txt')
install_data(noctalia.get_variable('noctalia_ui_tabler_license'), install_dir: license_dir)
install_data(noctalia.get_variable('noctalia_ui_wuffs_licenses'),
  install_dir: license_dir, rename: ['wuffs-LICENSE-APACHE', 'wuffs-LICENSE-MIT'])

# Keep uninstalled binaries on the same real asset bundle as installed binaries.
build_assets = meson.project_build_root() / 'assets'
run_command('mkdir', '-p', build_assets, check: true)
foreach bundle_dir : [
  translations_dir,
  noctalia_source_dir / 'assets' / 'emoji.json',
  noctalia_ui_assets_dir / 'fonts',
  meson.project_source_root() / 'assets' / 'lucide',
  meson.project_source_root() / 'assets' / 'sfx',
  meson.project_source_root() / 'assets' / 'icon.png',
]
  run_command('ln', '-sfn', bundle_dir, build_assets, check: true)
endforeach
