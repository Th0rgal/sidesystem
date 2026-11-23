#!/usr/bin/env bash
# deploy-overnight-updates.sh — Push updated sidesystem-ctl (thermal-smart powerd + wattage/temp JSON)
# and install SideSystem v6 APK in-place (preserving UID & Magisk root grant) once Razr is connected via USB.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CTL_SRC="${SCRIPT_DIR}/sidesystem-ctl"
SIGN_INSTALL="/home/thomas/work/nixos/nixos-config/hosts/razr/android/sign-and-install-apk.sh"
APK_V6="/tmp/sidesystem-v6/SideSystem-v6-raw.apk"

adb get-state >/dev/null 2>&1 || { echo "ERROR: Connect Razr via USB (adb) first." >&2; exit 1; }

echo "=== 1. Deploying updated sidesystem-ctl (pure-GPU direct Hyprland + thermal powerd + wattage/temp JSON) ==="
adb push "$CTL_SRC" /data/local/tmp/sidesystem-ctl
adb shell 'su -c "cp /data/local/tmp/sidesystem-ctl /data/local/nixos/bin/sidesystem-ctl && chmod 755 /data/local/nixos/bin/sidesystem-ctl && rm -f /data/local/nixos/powerd.pid && kill -9 \$(pgrep -f \"[s]idesystem-powerd\") 2>/dev/null || true"'

if [ -f "$APK_V6" ] && [ -x "$SIGN_INSTALL" ]; then
    echo "=== 2. Signing & installing SideSystem v6 APK in-place ==="
    "$SIGN_INSTALL" "$APK_V6"
fi

echo "=== 3. Live status verification ==="
adb shell 'su -c "/data/local/nixos/bin/sidesystem-ctl status --json"'
echo "DONE."
