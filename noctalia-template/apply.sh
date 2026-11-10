#!/bin/sh
# Noctalia already wrote the palette. Notify every instance owned by this user.
# BlastIRC also watches its palette every 250ms, covering missed/coalesced signals.
set -eu
status=0
pkill -USR1 -u "$(id -u)" -x blastirc || status=$?
# pkill's 1 means the application is not running; that is not a failed apply.
[ "$status" -le 1 ]
