#!/usr/bin/env bash
# SPDX-License-Identifier: 0BSD
# Remove the compat shim. The application's own files were never changed.
set -euo pipefail
APP="${THR_APP_DIR:-$HOME/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote}"
rm -f "$APP/version.dll"
wine reg delete 'HKCU\Software\Microsoft\Avalon.Graphics' /v DisableHWAcceleration /f >/dev/null 2>&1 || true
echo "removed shim from: $APP"
