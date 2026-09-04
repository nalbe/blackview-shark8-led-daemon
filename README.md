# led_hal_root — Notification LED daemon for Blackview Shark 8

KernelSU module that drives the AW2033 RGB notification/charge LED on a rooted Blackview Shark 8 running a ported Pixel GSI ROM. Single event-driven daemon (`chgd`) — no polling scripts.

## Install

1. Download [`led_hal_root_v2.10.zip`](https://github.com/nalbe/blackview-shark8-led-daemon/releases/download/v2.10/led_hal_root_v2.10.zip)
2. Flash in KernelSU Manager → Modules → Install from storage
3. Reboot

## What it does

| Mode | Behavior |
|------|----------|
| **Charge** | Red breathing <90%, lime flashing 90–94%, green solid ≥95% |
| **Notification** | Per-app RGB color, breathing, screen OFF only |
| **Incoming call** | Smooth rainbow while ringing |
| **Outgoing call** | Same rainbow for the call duration |
| **VoIP call** | Rainbow for Telegram/WhatsApp/Viber/Signal live calls |
| **Missed call** | Blue breathing (verified against call_log) |

All colors, thresholds, breathing timings and the suppress blacklist are user-editable in `led.conf` — no rebuild needed.

## Configure — LED GUI app

The easiest way to configure everything is the **LED GUI** companion app (see below). It shows live LED status, lets you pick colors with sliders, toggle the daemon and keepalive, view logs, and run test hooks — all from the phone. Edits save directly to `led.conf` and apply on the fly, no daemon restart.

For power users who prefer the terminal, you can edit `/data/adb/modules/led_hal_root/led.conf` by hand — same effect.

## Architecture

```
core.c    — main loop, select() over netlink + logdr, adaptive timer
led.c     — LED hardware writes (the ONLY RGB channel writer)
config.c  — led.conf parser + generic key-value store for mods
util.c    — logging, sysfs helpers, status file, screen detection

mods/
  charge.c   — charge band eval + LED application
  notify.c   — notification pipeline (suppress → color → arm/disarm)
  ring.c     — incoming/outgoing call rainbow
  dialer.c   — missed-call verification
  tele.c     — dumpsys capture helpers
  voip.c     — messenger VoIP call rainbow
```

Adding a feature = new file under `mods/`, no core edits. Extensions use `REGISTER_HANDLER` / `REGISTER_MODE` / `REGISTER_UEVENT` macros that place entries into linker sections.

## Build from source

Requires [Android NDK r27d](https://developer.android.com/ndk/downloads). Edit `NDK_CC` in `led_hal_root/build.cmd` if your NDK is elsewhere, then:

```
cd led_hal_root
build.cmd
```

Or compile directly:
```
aarch64-linux-android29-clang.cmd -O2 -s -Wall -Wno-comment -o chgd *.c mods/*.c
```

## Deploy

With the device connected and `adb root` working:

```
install_core.cmd
```

This rebuilds, pushes all module files, and restarts the daemon.

## Test hooks

```bash
kill -USR1 $(pidof chgd)   # fake Telegram notification
kill -HUP  $(pidof chgd)   # fake dialer ping (checks call_log)
kill -WINCH $(pidof chgd)  # force rainbow (test mode)
kill -QUIT  $(pidof chgd)  # cycle charge bands
kill -USR2  $(pidof chgd)  # disarm → back to charge
```

## LED GUI — companion app

The recommended way to use led_hal_root. A Kotlin/Android Views app (runs smooth at 120Hz on this firmware) with four swipeable tabs:

- **Info** — root status, daemon PID, keepalive toggle, live LED color swatch (reads RGB brightness 4x/sec), test hooks (fake Telegram / Dialer / Rainbow / Charge cycle / Disarm), log tail with logging toggle
- **Charge** — charge thresholds, per-range colors and light types (breathing / flashing / static), breath timings, soft-breath toggle
- **Notification** — default notification color, breath timings, notif max duration, screen delay
- **Call** — VoIP timeout, package list, ring test duration

All changes save to `led.conf` and take effect on the next daemon event — no restart, no rebuild.

### Install

Download [`led-gui.apk`](https://github.com/nalbe/blackview-shark8-led-daemon/releases/latest) and sideload it, or build from source:

```
install_gui.cmd
```

Requires root (KernelSU). The app will request root on first launch.

## Release history

See [PATCHNOTES.md](PATCHNOTES.md) for the full changelog.
