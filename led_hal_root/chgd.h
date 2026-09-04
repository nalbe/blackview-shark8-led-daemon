/*
 * chgd.h - shared API for the modular led_hal_root daemon.
 *
 * Layout:
 *   core (always compiled, never edit for a new feature):
 *     core.c      main loop, select(), netlink, logdr + events-buffer
 *                 decode (ev_parse/ev_load_tags), shared armed state
 *                 (g_st), pkg/uevent/refresh dispatch, adaptive timer
 *     led.c       LED hardware + rainbow cycler (the only writer)
 *     config.c    INI runtime config + generic kv for mod sections
 *     util.c      logging + sysfs helpers + status file + screen detect
 *   mods (drop a new .c into mods/, rebuild, done; pick and choose):
 *     charge.c    charge band eval + LED application. Owns the IDLE
 *                 channel: REGISTER_MODE owns the "" package (watchdog
 *                 heartbeat), REGISTER_UEVENT "power_supply" + refresh
 *     notify.c    notification pipeline. Claims every unclaimed package
 *                 via the "*" default handler and owns the ARMED
 *                 channel: REGISTER_MODE owns any real package (1s
 *                 heartbeat: timeout cap; disarm on cancel)
 *     ring.c      call rainbow mode (owns "incoming.call") - [ring] sec
 *     dialer.c    missed-call verification plugin (REGISTER_HANDLER,
 *                 the only REGISTER_RULE pseudo-package: "missed.call")
 *     tele.c      child-process capture + telephony probes (mods-only)
 *     voip.c      messenger (VoIP) call rainbow (owns "voip.call"),
 *                 driven by the audio-policy voice channel
 *
 * The registries below are linker-packed into dedicated sections;
 * the core iterates the section bounds, so a new feature never touches
 * core code. REGISTER_* entries must be static const data.
 *
 * Mode ownership is mutually exclusive by construction for every state
 * of g_st.cur_pkg: "" -> charge, "incoming.call" -> ring (notify defers
 * to it via ring_is_active()), any other package -> notify.
 */

#ifndef CHGD_H
#define CHGD_H

#include <stddef.h>
#include <time.h>

/* ---------------- logging (util.c) ---------------- */

extern int g_verbose;
void log_line(const char *fmt, ...);
#define LOGI(...) log_line(__VA_ARGS__)
/* runtime logging switch: driven by [led] logging in led.conf */
void log_set_enabled(int on);

/* ---------------- sysfs / device helpers ---------------- */

int  read_line(const char *path, char *out, size_t n);
void write_sys(const char *path, const char *val);
int  screen_on(void);
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, int type);

/* ---------------- LED hardware (led.c, the only writer) ---------------- */

void led_set(const char *c, int level);
void led_solid_rgb(int r, int g, int b);
void led_breathe_rgb(int r, int g, int b, int rise, int hold, int fall, int offt);
void leds_all_off(void);
void set_rgb(int r, int g, int b);
/* rainbow cycler: reset (start), one eased fade step per heartbeat,
 * current color for the status file */
void led_rainbow_reset(void);
void led_rainbow_step(void);
void led_rainbow_rgb(int *r, int *g, int *b);
/* software breathing: exact per-channel amplitude on the aw2033
 * (hardware breath ignores it). Start=set params, stop via leds_all_off.
 * phase_ms = one ramp duration; rise and fall are equal, no hold/off. */
void led_soft_breathe_rgb(int r, int g, int b, long phase_ms);
void led_soft_breathe_stop(void);

/* ---------------- child-process / telephony (tele.c) ---------------- */

size_t run_capture(char *const argv[], char *out, size_t cap);
int tele_ringing(void);   /* any line in mCallState==RINGING  */
int tele_active(void);    /* any line in RINGING or OFFHOOK   */

/* ---------------- shared state ---------------- */

struct notif_state {
    char   cur_pkg[96];   /* armed package, "" = idle */
    time_t armed_at;
    int    test;          /* armed from a test hook: ignore the screen */
};

extern struct notif_state g_st;
extern char g_applied_band[64];    /* last charge state applied (fp) */
extern int  g_tfd;                 /* adaptive timerfd            */
extern int  g_ld;                  /* logdr socket fd             */
extern long long g_last_missed_id;
extern int  g_call_checks_left;
extern time_t g_next_call_check;

/* re-check heartbeat for the missed-call verification window, ms */
#define CALL_RECHECK_MS 2000L

/* transport constants shared with the timer policy (core.c uses them
 * for the logdr reconnect backoff; charge.c reuses WATCHDOG_SEC as its
 * idle heartbeat) */
#define RETRY_SEC    3
#define WATCHDOG_SEC 300

/* notif_max_sec is runtime-configurable via [notify] notif_max_sec in
 * led.conf (config.c); default 1800s, 0 = unlimited. */

/* ---------------- runtime config hooks (config.c) ----------------
 * Early lookups the core consults for user-editable settings. The
 * generic kv lookups at the bottom let mods own their own [sections]
 * in led.conf without config.c knowing the keys exist.
 */
void conf_maybe_reload(void);
int  conf_suppressed(const char *pkg);
int  conf_pkg_rgb(const char *pkg, int *r, int *g, int *b); /* 1 = set  */
int  conf_first_threshold(void);
int  conf_second_threshold(void);
void conf_charge_lower_rgb(int *r, int *g, int *b);   /* lower_range_color */
void conf_charge_middle_rgb(int *r, int *g, int *b);  /* middle_range_color */
void conf_charge_upper_rgb(int *r, int *g, int *b);   /* upper_range_color */
int  conf_charge_lower_type(void);                    /* 0..3 light type   */
int  conf_charge_middle_type(void);
int  conf_charge_upper_type(void);
void conf_notif_rgb(int *r, int *g, int *b);         /* [notify] default_color */
int  conf_notif_type(void);                           /* [notify] notify_light_type */
void conf_charge_timing(int *rise, int *hold, int *fall, int *offt);
void conf_notif_timing(int *rise, int *hold, int *fall, int *offt);
long conf_notif_max_sec(void);
/* generic kv: any [section] key=value in led.conf is readable by name */
const char *conf_get_str(const char *sec, const char *key); /* NULL = absent */
long conf_get_int(const char *sec, const char *key, long def);

/* ---------------- core services ---------------- */

void retune_timer(void);
const struct led_mode *mode_owns(const char *pkg);
/* events-buffer decode + logdr tag table - core-owned transport */
#define EV_NONE    0
#define EV_ENQUEUE 1
#define EV_CANCEL  2
void ev_load_tags(void);
int  ev_parse(const unsigned char *buf, size_t n, char *pkg, size_t pkgn);
/* pkg dispatch: exact-match claiming handlers first (dialer), then the
 * "*" default handler (notify). Returns 1 if something claimed it. */
int  pkg_dispatch(struct notif_state *st, const char *pkg);
/* uevent hooks: every REGISTER_UEVENT entry whose match substring is
 * present in the raw netlink message fires. */
void uev_dispatch(const char *msg);
/* refresh hooks: every REGISTER_REFRESH entry fires (boot + SIGALRM). */
void refresh_dispatch(void);
long eval_and_write(void);
void apply_charge_leds(void);
void arm_notification(struct notif_state *st, const char *pkg);
void arm_notification_ex(struct notif_state *st, const char *pkg, int test);
void disarm_notification(struct notif_state *st, const char *why);
void notify_pending_check(void);    /* flash late notify once screen is off */

/* ---------------- extension entry points ---------------- */

int  ring_is_active(void);          /* ring mode armed right now */
void arm_ring(int incoming);        /* 1 = incoming, 0 = outgoing */
void arm_ring_ex(int incoming, int test);   /* test=1: hold, ignore telephony */
int  voip_active(void);             /* messenger-call rainbow armed now */
int  voip_try(const char *pkg);     /* 1 if voip claimed the notification */
void arm_charge_test(void);         /* cycle all charge bands once (SIGQUIT) */
void maybe_call_check(void);
void dialer_reopen_window(void);    /* reopen missed-call verify window */
const char *dialer_pkg_id(void);    /* "com.google.android.dialer" */

/* ---------------- registries ---------------- */

struct led_rule {          /* internal pseudo-package color only      */
    const char *pkg;       /* (e.g. dialer's "missed.call"); real app */
    unsigned char r, g, b; /* colors live in led.conf [rules] only   */
};
struct pkg_handler {       /* full owner of a package's notifications */
    const char *pkg;       /* exact package-name match only          */
    int (*fn)(const char *pkg);   /* return 1 to claim the event     */
};
struct led_mode {          /* timer-driven special state */
    const char *name;
    long        tick_ms;   /* heartbeat while armed     */
    int        (*owns)(const char *pkg);
    void       (*tick)(void);
    /* optional adaptive wakeup: when set and >0, the core uses its return
     * value (ms) as a one-shot "sleep until this much later" instead of the
     * fixed tick_ms ticking every cycle. Re-evaluated after every tick, so a
     * mode can sleep for minutes when there is nothing to do and only wake
     * for the next actual deadline (e.g. a timeout cap). NULL = periodic
     * tick_ms heartbeat, the old behaviour. */
    long       (*next_wake_ms)(void);
};
struct uevent_hook {       /* netlink KOBJECT_UEVENT consumer */
    const char *match;     /* substring of the raw message    */
    void       (*fn)(void);
};
struct refresh_hook {      /* visible-state refresher: boot + SIGALRM */
    void (*fn)(void);
};

#define _CHG_CAT2(a, b) a##b
#define _CHG_CAT(a, b)  _CHG_CAT2(a, b)

#define REGISTER_RULE(_pkg, _r, _g, _b) \
    static const struct led_rule \
    _CHG_CAT(chg_rule_, __COUNTER__) \
    __attribute__((used, section("chgd_rules"))) = { (_pkg), (_r), (_g), (_b) }

#define REGISTER_HANDLER(_pkg, _fn) \
    static const struct pkg_handler \
    _CHG_CAT(chg_hand_, __COUNTER__) \
    __attribute__((used, section("chgd_handlers"))) = { (_pkg), (_fn) }

#define REGISTER_MODE(_name, _tick_ms, _owns, _tick) \
    static const struct led_mode \
    _CHG_CAT(chg_mode_, __COUNTER__) \
    __attribute__((used, section("chgd_modes"))) = \
        { (_name), (_tick_ms), (_owns), (_tick), NULL }

/* like REGISTER_MODE, but with an adaptive wakeup fn (see struct led_mode) */
#define REGISTER_MODE_WAKE(_name, _tick_ms, _owns, _tick, _wake) \
    static const struct led_mode \
    _CHG_CAT(chg_mode_, __COUNTER__) \
    __attribute__((used, section("chgd_modes"))) = \
        { (_name), (_tick_ms), (_owns), (_tick), (_wake) }

#define REGISTER_UEVENT(_match, _fn) \
    static const struct uevent_hook \
    _CHG_CAT(chg_uev_, __COUNTER__) \
    __attribute__((used, section("chgd_uevents"))) = { (_match), (_fn) }

#define REGISTER_REFRESH(_fn) \
    static const struct refresh_hook \
    _CHG_CAT(chg_refr_, __COUNTER__) \
    __attribute__((used, section("chgd_refresh"))) = { (_fn) }

/* section bounds, synthesized by the linker around the registry data */
extern const struct led_rule        __start_chgd_rules[];
extern const struct led_rule        __stop_chgd_rules[];
extern const struct pkg_handler     __start_chgd_handlers[];
extern const struct pkg_handler     __stop_chgd_handlers[];
extern const struct led_mode        __start_chgd_modes[];
extern const struct led_mode        __stop_chgd_modes[];
extern const struct uevent_hook     __start_chgd_uevents[];
extern const struct uevent_hook     __stop_chgd_uevents[];
extern const struct refresh_hook    __start_chgd_refresh[];
extern const struct refresh_hook    __stop_chgd_refresh[];

#endif /* CHGD_H */