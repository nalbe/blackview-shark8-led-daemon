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
  - `build.cmd` — compile `chgd` with `set NDK=...` (see below)
- `apply.cmd` — rebuild + push module files to the device + restart the stack
- `PATCHNOTES.md` — full revision history

## Build

Requires the Android NDK (clang, aarch64-linux-android29). Set the NDK
root once, then:

```
set NDK=D:\path\to\android-ndk-r27d
led_hal_root\build.cmd
```

## Install / apply (device connected, adb root working)

```
apply.cmd
```

Or push the module by hand per `led_hal_root/README.txt`.

## Config

`/data/adb/modules/led_hal_root/led.conf` is editable at runtime — colors,
charge thresholds/light types, and the suppress blacklist apply on the next
event without a rebuild or restart.

Full behavioral details and test hooks: `led_hal_root/README.txt` and
`PATCHNOTES.md`.
