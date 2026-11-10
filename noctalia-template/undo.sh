#!/bin/sh
# Only remove the generated theme; identities, settings, and history are untouched.
set -eu
config=${XDG_CONFIG_HOME:-"$HOME/.config"}
rm -f -- "$config/blastirc/noctalia.toml"
exec "$(dirname -- "$0")/apply.sh"
