# led_hal_root (Blackview Shark 8)

Root module that drives the notification / charge LED on the Blackview
Shark 8 running a ported Pixel ROM (MediaTek / Android 14). The stock MTK
lights HAL cannot drive the AW2033 RGB LED on this hardware, so a single
event-driven native daemon (`chgd`) takes over the `/sys/class/leds/{red,
green,blue}` nodes directly.

No polling loops: `chgd` subscribes to the logd events socket
(`notification_enqueue`) and kernel uevents, and uses an adaptive one-shot
timerfd only as a fallback. A small supervisor (`keepalive.sh`) restarts it
if it ever dies.

## Contents

- `led_hal_root/` — the module itself
  - `chgd` / `*.c` / `mods/` — the daemon sources (modular: core + extensions)
  - `service.sh` / `keepalive.sh` / `customize.sh` / `META-INF` — boot stack + module scaffolding
  - `led.conf` — runtime config (colors, charge bands, suppress blacklist)
  - `build.cmd` — compile `chgd` (override compiler with `set NDK_CC=...`)
- `led_gui/` — Kotlin/Compose root helper app (`com.bastet.ledgui`)
- `led-gui.apk` — prebuilt GUI app, ready to sideload
- `apply.cmd` — rebuild + push the daemon module to the device + restart the stack
- `install_gui.cmd` — build (if needed) + install the GUI app
- `PATCHNOTES.md` — full revision history

## Build the daemon

Requires the Android NDK (clang, aarch64-linux-android29). Set the compiler
once, then:

```
set NDK_CC=D:\path\to\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd
led_hal_root\build.cmd
```

## Build the GUI

Requires Android Studio / Gradle (JDK 17). From `led_gui/`:

```
gradle assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/app-debug.apk`.

## Install / apply (device connected, adb root working)

```
apply.cmd          # rebuild + push the daemon module
install_gui.cmd    # build + install the GUI app
```

Or push the module by hand per `led_hal_root/README.txt`.

## Config

`/data/adb/modules/led_hal_root/led.conf` is editable at runtime — colors,
charge thresholds/light types, and the suppress blacklist apply on the next
event without a rebuild or restart. The GUI app also edits this file and
shows the live LED state (from `/data/local/tmp/led_status`).

Full behavioral details and test hooks: `led_hal_root/README.txt` and
`PATCHNOTES.md`.
