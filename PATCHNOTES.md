# led_hal_root patch notes (2026-08-23)

## Revision: full RGB colors, masks removed (2026-08-29)
1. Colors are now RGB triplets (0-255 per channel) end to end instead of
   the bit masks (4=red, 2=green, 1=blue). Per-channel brightness is
   written into each LED node, so any color is expressible, not just the
   7 mask combinations (and the AW2033 blue-dominated diode is no longer
   hardcoded to the lens).
2. led.conf schema (v2.7) - replace the old mask values:
   - `[rules] pkg=r,g,b`          (was `pkg=color-mask`)
   - `[charge] first_threshold / second_threshold` (percent, order-free -
     swapped automatically)
   - per-range light types `lower/middle/upper_range_light_type`
     (0=off 1=breathing 2=flashing 3=static) and colors
     `lower/middle/upper_range_color=r,g,b` (band colors were hardcoded
     red/amber/green in charge.c)
   - `[notify] default_color=r,g,b` (white fallback for unlisted apps)
   Old-style single-number values are rejected and logged, not misparsed.
3. Range abstractions: thresholds are `first_threshold`/`second_threshold`,
   ranges are lower/middle/upper end to end (state file, logs, conf), and
   the behavior per range is explicitly configurable - previously upper
   was always solid and the names amber_at/green_at lied once the colors
   changed. "middle" defaults to flashing (square blink, fades forced 0).
4. Applied-charge cache is now a fingerprint (band + color + light type +
   timings): editing led.conf colors/types while charging re-applies on
   the next event instead of waiting for a band change. The "none" band
   also explicitly turns all channels off now.
5. Sources: struct led_rule / REGISTER_RULE take r,g,b; util.c gained
   led_solid_rgb / led_breathe_rgb (led_set now writes any level 0-255);
   notify.c resolves rgb_for() per package; charge.c reads its per-range
   colors and light types from conf.c. Defaults: lower breathing red,
   middle flashing lime, upper static green.
6. Bumped module to v2.7 / code 9.

## Revision: stale legacy LED daemons (2026-08-28)
Second "spurious blue/purple" report (no `notify armed` in the log, yet
green+blue channels lit at 255 with led_time "15 15 15 15" - the white
pattern reads blue/purple on the blue-dominated AW2033). Root cause:
orphaned legacy daemons `worker.sh` and `listener.sh` from the OLD pre-chgd
module stack. They survived the module update as PPid=1 and kept writing
their own colour into the RGB sysfs nodes, fighting the single chgd daemon.
Fix: killed both by hand once. keepalive stays at v7 - it does NOT scan for
these legacy names by design: the files no longer exist in the module and
v1.3 service.sh never launches them, so they cannot come back. No reaping
logic, no wasted cycles.

## Revision: background-notification blacklist (2026-08-28)
Spurious colour switch observed while charging: the red charge breath changed
to blue/purple on its own and only went back to red after waking the display.
Root cause was NOT an internal LED bug - the default white (mask 7) for
unlisted packages lights red+green+blue together, and on the AW2033 the blue
channel dominates so it reads as blue/purple. `com.google.android.google
quicksearchbox` (Google Discover) posts such background feed notifications
spontaneously while the screen is off; they claim the LED away from the
charge indication (notifications take priority) and only drop it again on a
screen-on disarm tick.
Fix: expanded the suppress blacklist with these quiet background emitters
so they can never steal the LED:
- com.google.android.googlequicksearchbox   (Discover / Google app - #1)
- com.android.providers.media.module        (media/storage scanning)
- com.google.android.apps.nbu.files         (Google Files space hints)
Any further noise: add one REGISTER_SUPPRESSED line in mods/suppress.c and
rebuild. Verified against installed packages (superseded viber.voip, which is
not present, and dropped non-installed Assistant/TV/Chromecast entries).

## Revision: ColorNote night sync blacklist (2026-08-29)
"LED breathing blue/purple/white at night for no reason" report (no charger
connected, screen off, user asleep). ledd.log showed exactly ONE armed event
overnight: `09:43:00 notify armed: com.socialnmobile.dictapps.notepad.color
.note mask=7`. logcat pinned it - ColorNote started a background
`DailySyncJobService` + `SyncService` (nightly auto backup/sync of notes) and
posted a mask=7 notification (red+green+blue = white, reads blue/purple on
the AW2033). No visible user notification, no charger, so it looks
spontaneous to the user.
Fix: added com.socialnmobile.dictapps.notepad.color.note to the suppress
blacklist so its night sync can never steal the LED.

## Revision: runtime config, no-recompile editing (2026-08-29)
The blacklist, per-app colours, breathing timings and charge thresholds are
now user-editable in a plain text file:
`/data/adb/modules/led_hal_root/led.conf` (shipped with the module).
Sections: [suppress] (one package per line - never lights the LED),
[rules] (pkg=color-mask), [charge] (amber_at/green_at + breath ms),
[notify] (breath ms + notif_max_sec).
- New mods/conf.c parses it; runtime entries MERGE OVER the link-time
  registries (files win on conflicts, suppress adds). No recompile needed.
- Lazy reload: conf_maybe_reload() stat()s the file and only re-reads when
  the mtime changed, so editing led.conf applies on the next processed
  event - no daemon restart, no rebuild. Verified live: adding telegram to
  [suppress] took effect on the very next enqueue without a restart.
- Core hooks: chgd.h declares conf_* getters; notify.c (mask_for,
  suppressed, arm_notification timings), charge.c (band thresholds +
  breath timing) and chgd.c (notif_max_sec) now consult them. Builtin
  defaults unchanged when the file is absent.

## Revision: modular source layout (2026-08-28)
1. chgd.c is no longer a monolith. Sources split into a stable CORE
   (chgd.c / chgd.h / util.c / tele.c / notify.c / charge.c) and
   extension MODS (mods/rules.c, mods/suppress.c, mods/ring.c,
   mods/dialer.c). Behavior is byte-for-byte the v2.6 logic.
2. Extension mechanism: REGISTER_RULE / REGISTER_SUPPRESSED /
   REGISTER_HANDLER / REGISTER_MODE macros. Each entry is a static
   const struct placed by __attribute__((section)) into chgd_rules /
   chgd_suppressed / chgd_handlers / chgd_modes; the core iterates the
   linker-synthesized __start_/__stop_ bounds. A new app color/handler
   = a new .c file under mods/ and a rebuild. No core edits, ever.
3. build.cmd compiles the core + every mods/*.c in one NDK clang call
   (single static binary). apply.cmd now rebuilds before pushing.
4. Old single-file sources archived under legacy/ as a fallback.

## Revision: outgoing-call rainbow (2026-08-27)
1. LED never lit on OUTGOING calls. The old trigger only armed the
   rainbow when mCallState==1 (RINGING / incoming), so dialing out
   (mCallState==2 / OFFHOOK) did nothing.
2. Fix: dialer posts a notification_enqueue for outgoing calls too
   (channel phone_ongoing_call). That same event-time probe now uses
   tele_active() (mCallState 1 OR 2) instead of tele_ringing() (only 1),
   so an active outgoing call arms the same rainbow. Still fully
   event-driven - a dialer ping triggers a one-shot dumpsys probe, no
   polling loop added.
3. Call direction is recorded (g_ring_incoming, from a tele_ringing()
   probe at arm time) so the end-resolution differs: incoming ->
   missed-call check (blue breath), outgoing -> straight back to the
   charge leds (a missed row can never appear for an outgoing call).
4. Bumped module to v2.6 / code 8.

## Revision: field-fixes (2026-08-24)
1. Rainbow died ~0.5s into a real ring: this ROM wakes the display for
   incoming calls and the screen-on guard killed the effect. Ring mode
   now ignores screen state completely (fb/lcd uevent path included);
   exits only on ring end or RING_MAX_SEC (120s).
2. Telegram breathing "stopped after a while" = NOTIF_MAX_SEC timeout,
   by design (was 600s, worker.sh parity). Raised to 1800s.

## Revision: incoming-call rainbow
Dialer pings are now cross-checked against telephony state
(dumpsys telephony.registry -> mCallState, any line):
- mCallState==RINGING at ping time (or during the verification window)
  -> rainbow mode: timer drops to 32ms; each tick interpolates toward
   the next of 8 palette anchors along a cosine-eased curve (16 steps
   per segment, ~4.1s full cycle), written straight into
   {red,green,blue}/brightness - colors glide, no hard switching.
   First state re-poll is deferred by RING_CHK_SEC so an instant
   hang-up still gets a short glow instead of a zero-length flash.
- Ring end (state leaves RINGING, polled every 1s) -> resolve outcome:
  fresh missed row -> blue missed-call breath; answered/reset ->
  back to charge leds. Screen-on disarms immediately as usual.
- NOTIF_MAX_SEC cap applies; repeated dialer pings during an active
  ring are ignored (no rainbow restart).
Test hooks: SIGWINCH = force rainbow (bypasses state check; on an idle
device it ends after the first 170ms tick - by design, real ringing
keeps it alive). Verify live with an actual incoming call.

## Revision: missed-call indication
Dialer pings (com.google.android.dialer) no longer arm the LED blindly.
Every dialer notification is verified against call_log via root
`content query`: a fresh (<=120s) MISSED row (type=3, new=1) arms blue
breathing; ringing/ongoing-call notifications are ignored. Verification
window: up to 4 checks x 2s (absorbs provider/db race), dedup by call_log
_id (notification updates do not re-arm). Timer policy gains a 2s
"call check" state between armed(1s)/retry(3s)/watchdog(300s).
Test hook: kill -HUP $(pidof chgd) = fake dialer ping (real call_log
check runs; insert a type=3/new=1 row to see it fire).
Note: this ROM's call_log provider rejects insert binds for description
and country columns; use type,new,number,date,duration only.

## Revision: trigger-driven timer (same day)
Replaced the fixed 30s timerfd with an adaptive single-shot policy:
- notification armed -> 1s (screen re-check; NOTIF_MAX_SEC cap unchanged)
- logdr disconnected -> 3s reconnect backoff (was: up to 30s dead air)
- idle, links up     -> 300s watchdog only (missed-uevent safety net)
All real transitions were already trigger-driven (netlink uevents for
power_supply/fb0, logdr stream for notifications); the tick is now just a
fallback and is re-armed via retune_timer() on arm/disarm/logdr drop.
Steady idle state: zero periodic LED work between watchdog ticks.

## What this is
KernelSU module "led_hal_root": single event-driven daemon (chgd) that drives
the notification/charge LEDs on a rooted MediaTek Android 14 device.
Replaces the old polling script stack (worker.sh + listener.sh + chgd v1).

## Components
- chgd          compiled daemon (aarch64, NDK r27d, android29)
- chgd.c        full source
- service.sh    module boot entry: starts chgd + keepalive after 8s delay
- keepalive.sh  v7 supervisor: restarts chgd if it dies; single-instance via /proc scan
- module.prop / customize.sh / META-INF   standard KernelSU module scaffolding

## Key fixes in this revision
1. Daemon hang: select() returned EINTR with a stale fd_set after switching to
   sigaction without SA_RESTART -> blocking recv on netlink forever.
   Netlink socket is now O_NONBLOCK, EINTR restarts the loop cleanly,
   uevents are drained with MSG_DONTWAIT.
2. Real notification events: logdr stream ("stream tail=1 lids=2", numeric
   tag ids only). This ROM writes event-log payloads little-endian and the
   notification_enqueue record carries an extra zero byte vs other tags, so
   strict schema walking is unreliable. Parser now anchors on the self-
   describing string record [0x02][u32le len][pkg][NUL] after matching tag id
   (loaded from /system/etc/event-log-tags at startup).
3. LED flicker every ~30s: tick/battery uevents rewrote led nodes even when
   nothing changed. Applied-band cache now rewrites nodes only on real band
   transitions; arm_notification invalidates the cache.
4. keepalive duplicates: pidfile check raced and went stale. v7 scans
   /proc/*/cmdline for any live instance instead.

## Behavior
- Charge bands: Full->green solid; Charging <90% red breathe, 90-94 amber
  (red+green), >=95 green solid; Not charging >=95 green else none.
- Notifications: only when screen is OFF; suppressed packages list;
  per-app color masks (telegram = red+blue "purple" mask 5);
  disarm on screen-on tick or after 600s.
- Screen detection: lcd-backlight brightness, fb0 blank fallback.
- Test hooks: kill -USR1 $(pidof chgd) = fake telegram notification,
  kill -USR2 = disarm.

## Rebuild from source
Run build.cmd (compiles core + mods/*.c into chgd). Point it at your NDK
with `set NDK=path\to\android-ndk-r27d` (or `set NDK_CC=path\to\...
\aarch64-linux-android29-clang.cmd`) before running. Single-file fallback
for the archived monoliths is described in led_hal_root/README.txt.

## Apply (device connected, adb root working)
apply.cmd  — pushes module files, fixes perms, restarts the stack.
