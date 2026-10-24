#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Build the compat shim and put it next to THR Remote.exe.
# Nothing of the application is modified: the shim is a stand-in version.dll that Wine loads through a
# DLL override and that redirects a few imports in memory (see thr_compat.c).
# Needs: gcc-mingw-w64-i686, an installed THR Remote in the Wine prefix.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
APP="${THR_APP_DIR:-$HOME/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote}"
[ -f "$APP/THR Remote.exe" ] || { echo "THR Remote.exe not found in: $APP" >&2; exit 1; }

i686-w64-mingw32-gcc -shared -O2 -Wall -o "$here/version.dll" "$here/thr_compat.c" "$here/thr_compat.def" \
    -Wl,--kill-at -lwinmm
cp "$here/version.dll" "$APP/version.dll"
rm -f "$here/version.dll"

# WPF cannot compile its shaders under Wine; use software rendering.
wine reg add 'HKCU\Software\Microsoft\Avalon.Graphics' /v DisableHWAcceleration /t REG_DWORD /d 1 /f >/dev/null

echo "installed: $APP/version.dll"
