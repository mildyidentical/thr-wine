#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Start THR Remote under Wine.
# The amp only answers SysEx while its USB audio capture stream is open (the Windows driver keeps one
# open), so we hold a capture stream on it for as long as the app runs.
#
# The capture goes through PipeWire (not raw ALSA) so PipeWire keeps the device at its own sample rate;
# opening the card directly at 44.1 kHz while playback runs at 48 kHz can upset the amp's USB audio.
set -euo pipefail
APP="${THR_APP_DIR:-$HOME/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote}"

node="$(pw-cli ls Node 2>/dev/null | sed -n 's/.*node.name = "\(alsa_input\.usb-Yamaha.*THR[0-9]*II.*\)"/\1/p' | head -1)"
[ -n "$node" ] || { echo "THR amp not found in PipeWire (USB connected and powered on?)" >&2; exit 1; }

pw-record --target "$node" --rate 48000 --channels 2 --format s16 /dev/null >/dev/null 2>&1 &
keepalive=$!
cleanup() { kill "$keepalive" 2>/dev/null || true; }
trap cleanup EXIT
sleep 1

cd "$APP"
# Load the compat shim (version.dll next to the exe) instead of Wine's built-in one.
export WINEDLLOVERRIDES="version=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
# Not exec'd: let the app quit on its own and then release the capture stream.
wine "THR Remote.exe"
