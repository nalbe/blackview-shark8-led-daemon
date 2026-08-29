led_hal_root - notification + charge LED daemon
================================================

ARCHITECTURE (v2.7, MODULAR)
---------------------------
One native daemon "chgd" does everything. No polling scripts.
Sources are split into a CORE (written once) and MODS (extension
files). Adding a feature never touches the core:

  core (always compiled):
    chgd.c      main loop: select() over netlink uevents,
                /dev/socket/logdr stream ("tail=1 lids=2",
                notification_enqueue parsing), adaptive timerfd
                (retune_timer/retune), signal test hooks
    chgd.h      shared API + registry macros (REGISTER_RULE /
                REGISTER_HANDLER / REGISTER_MODE)
    util.c      logging, sysfs LED primitives, screen detection
    tele.c      one-shot dumpsys/child capture helpers
    notify.c    enqueue dispatch: suppressed list -> claiming
                handlers -> color rules -> arm/disarm breathing
    charge.c    charge band eval, state file, LED application

  mods/ (extension files, pick and choose):
    ring.c      incoming/outgoing call rainbow (a timer MODE)
    dialer.c    missed-call verification plugin (claims all dialer
                notifications via REGISTER_HANDLER)

Per-app colors and the suppress blacklist are runtime-only (led.conf),
no rebuild needed. The only remaining link-time rule is the internal
pseudo-package registered by dialer.c ("missed.call" = blue).

Registries are packed into linker sections: each REGISTER_* entry is
a static const struct in .chgd_rules/.chgd_handlers/
.chgd_modes, and the core iterates the section bounds. New file in
mods/ = new entries, no core edit.

MODULAR FLOW
------------
Add a claiming handler: create mods/myapp.c with one line
      REGISTER_HANDLER("com.example.app", my_handler_fn);
or a timer mode:
      REGISTER_MODE("mymode", 100, my_owns, my_tick);
then run build.cmd and apply. User-facing colors/suppress/charge/notify
settings all live in led.conf and apply on the fly without a rebuild.
The whole daemon still compiles into one static binary.

BOOT STACK
----------
  service.sh      module entry, launched by KernelSU at boot; sleeps 8s,
                  starts chgd and keepalive.sh
  keepalive.sh    v7 supervisor: every 30s restarts chgd if dead;
                  single-instance enforced via /proc/*/cmdline scan

LED NODES
---------
/sys/class/leds/{red,green,blue}/{brightness,blink,led_time}
Hardware breathing via blink=1 + led_time "RISE HOLD FALL OFFT" (ms,
quantized by aw2033 driver). IMPORTANT: stale led_time keeps pulsing -
every solid/all-off write must clear blink and led_time first.

COLORS
------
All colors are RGB triplets (0-255 per channel), runtime-configured in
led.conf: [rules] pkg=r,g,b, [notify] default_color=r,g,b,
[charge] lower/middle/upper_range_color=r,g,b with per-range light type.
There are NO builtin per-app colors anymore - the config is the single
source (mods/rules.c was removed). Builtin fallbacks remain only in
mods/conf.c for [charge]/[notify] values when a key is absent.

Notifications (screen OFF only), colors from led.conf [rules]:
  com.whatsapp                              green         0,255,0
  org.telegram.messenger                    purple        255,0,255
  everything else                           [notify] default_color
Missed calls (verified via root content query on call_log:
type=3 new=1, fresh <=120s, dedup by _id):
  com.google.android.dialer -> missed.call  blue          0,0,255
Incoming call while RINGING (mCallState, dumpsys telephony.registry):
  smooth rainbow: colors cross-fade between 8 palette anchors over
  16 cosine-eased steps each, one step per 32ms (~4.1s full cycle),
  until the ring ends; outcome resolved automatically
  (blue breath / charge leds)
Outgoing call while OFFHOOK (mCallState==2, same event-triggered probe):
  identical rainbow for the whole call; on end just drops back to the
  charge leds (no missed-call check for outgoing calls).
Suppressed (led.conf [suppress] only, plus nothing else - the builtin
list in mods/suppress.c was removed): com.android.systemui, android,
com.android.shell, org.amnezia.vpn, and the quiet background emitters.
The rainbow palette lives in mods/ring.c, call handling in mods/dialer.c.

Charge bands. Ranges are named abstractly (lower/middle/upper) and the
behavior per range - light type (0=off 1=breathing 2=flashing 3=static)
and color - is configured in led.conf [charge]:
  Full                                      upper: static green (0,255,0)
  Charging, below first (90)                lower: breathing red (255,0,0)
  Charging, first..second (90-94)           middle: flashing lime (96,255,0)
  Charging, >= second (95)                  upper: static green
  Not charging, >= 95                       upper: static green
Thresholds (first_threshold/second_threshold, order-free - swapped
automatically), light types (lower/middle/upper_range_light_type) and
colors (lower/middle/upper_range_color) in led.conf [charge], builtin
defaults in mods/conf.c. Edits re-apply on the next event: the applied
cache is a fingerprint of band+color+type+timings.

STATE FILES (/data/local/tmp/)
------------------------------
ledd.log             daemon log
chgd.err             daemon stderr
led_chg              "<band> <epoch>" current charge band
led_chgd.lock        flock lock
led_keepalive.pid    keepalive pid (informational)

REBUILD
-------
build.cmd compiles core + every .c under mods\ into chgd.
Custom NDK_CC env var overrides the compiler path.

APPLY MANUALLY
--------------
adb push chgd $MOD/ && adb shell chmod 755 $MOD/chgd
then restart: kill -9 $(pidof chgd); setsid sh $MOD/service.sh
or just run apply.cmd from the package root (rebuilds + pushes
everything + restarts).

TESTING
-------
Screen off, then: kill -USR1 $(pidof chgd)   -> purple breathing
                 kill -HUP  $(pidof chgd)   -> blue breathing if call_log
                                                has a fresh missed row
                 kill -WINCH $(pidof chgd)  -> rainbow (one step on idle
                                                device; real ring keeps it)
Wake screen                          -> disarmed by fb uevent or <=1s tick
su 2000 -c "cmd notification post -t x shelltag y"
                                     -> suppressed (com.android.shell)

LEGACY
------
The pre-modular monoliths are archived in legacy/:
  chgd_monolithic_v2.6.c   the former single-file chgd.c
  chgd_dbg.c               the older debug-variant daemon
Rebuild them by hand if you ever need a fallback:
  aarch64-linux-android29-clang.cmd -O2 -s -o chgd legacy/chgd_monolithic_v2.6.c

KNOWN QUIRKS OF THIS DEVICE/PORT
--------------------------------
- event payloads are LITTLE-ENDIAN; enqueue records carry an extra zero
  byte vs other tags -> parser anchors on the [02][len][pkg][NUL] record
- logdr accepts numeric lids only; without tail=1 it replays the whole
  backlog (~5k packets)
- keyevent 223 dozes, 224 wakes (inverted vs docs on this build)
- reading /sys/class/leds/*/blink as root shell gives EACCES but writes
  succeed from the daemon context