# SideSystem

Minimalist, Orb-inspired Android companion app and native Linux bridge for running a full **NixOS + Hyprland** desktop container side-by-side with Android (via `droidspaces` on a namespace-enabled GKI kernel).

## Highlights

- **Orb Obsidian UI (`#141414`)**: Calm 3-zone interface with an expandable live CPU/RAM telemetry chart (floating translucent options pill for series filter, poll interval, and pause/resume).
- **Razr 60 Ultra Cover-Screen Native**: Full-bleed `moto_cli_fullscreen_mode` layout that flows around the twin bottom-right camera lenses with a bottom-left floating pill dock.
- **Any Resolution & Scale**: Quick presets (`720p`, `1080p`, `1440p`, `3440×1440`, `4K`) plus arbitrary `W×H` custom resolution and `0.5x` / `1x` / `1.5x` / `2x` scaling.
- **Exclusive Bluetooth HID Bridge (`bt-hid-bridge`)**: 84 KB static ARM64 C daemon that grabs Bluetooth keyboards & mice (`EVIOCGRAB` on `BUS_BLUETOOTH` `/dev/input/event*`) so they control NixOS directly instead of Android, with one-tap toggle back to Android.
- **Zero-Termux Native Backend (`sidesystem-ctl`)**: Pure Android Toybox `/system/bin/sh` CLI controlling container lifecycle, headless `sway` + `Hyprland` + `wayvnc`, SSH (`:2222`), 4G/Wi-Fi networking, clipboard sync, restic backups, and logs.

## Layout

- `android-app/` — Kotlin + Jetpack Compose Material 3 Android app (`com.th0rgal.sidesystem`)
- `scripts/sidesystem-ctl` — Android-side POSIX shell control CLI (`/data/local/nixos/bin/sidesystem-ctl`)
- `scripts/bt-hid-bridge.c` — Static ARM64 Bluetooth HID (`evdev`) → `wayvnc` bridge
- `scripts/sign-and-install-apk.sh` — Local persistent keystore re-signer for in-place `adb install -r` updates

## Build & Install

```bash
cd android-app
gradle --no-daemon :app:assembleRelease
../scripts/sign-and-install-apk.sh app/build/outputs/apk/release/app-release.apk
```
