/*
 * chgd.h - shared API for the modular led_hal_root daemon.
 *
 * Layout:
 *   core (always compiled, never edit for a new feature):
 *     chgd.c      main loop, select(), netlink, logdr, timer policy
 *     util.c      logging + sysfs LED primitives + screen detection
 *     tele.c      child-process capture + telephony probes
 *     notify.c    notification dispatch + events-buffer parser
 *     charge.c    charge band evaluation + LED application
 *   extensions (drop a new .c into mods/, rebuild, done):
 *     mods/ring.c     call rainbow mode
 *     mods/dialer.c   missed-call verification plugin (owns the only
 *                     REGISTER_RULE pseudo-package: "missed.call")
 *
 * The registries below are linker-packed into dedicated sections;
 * the core iterates the section bounds, so a new feature never touches
 * core code. REGISTER_* entries must be static const data.
 */

#ifndef CHGD_H
#define CHGD_H

#include <stddef.h>
#include <time.h>

/* ---------------- logging (util.c) ---------------- */

extern int g_verbose;
void log_line(const char *fmt, ...);
#define LOGI(...) log_line(__VA_ARGS__)

/* ---------------- sysfs / device helpers (util.c) ---------------- */

int  read_line(const char *path, char *out, size_t n);
void write_sys(const char *path, const char *val);
void led_set(const char *c, int level);
void led_solid_rgb(int r, int g, int b);
void led_breathe_rgb(int r, int g, int b, int rise, int hold, int fall, int offt);
void leds_all_off(void);
void set_rgb(int r, int g, int b);
int  screen_on(void);

/* ---------------- child-process / telephony (tele.c) ---------------- */

size_t run_capture(char *const argv[], char *out, size_t cap);
int tele_ringing(void);   /* any line in mCallState==RINGING  */
int tele_active(void);    /* any line in RINGING or OFFHOOK   */

/* ---------------- shared state ---------------- */

struct notif_state {
    char   cur_pkg[96];   /* armed package, "" = idle */
    time_t armed_at;
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

/* notif_max_sec is runtime-configurable via [notify] notif_max_sec in
 * led.conf (mods/conf.c); default 1800s, 0 = unlimited. */

/* ---------------- runtime config hooks (mods/conf.c) ----------------
 * Early lookups the core consults for user-editable settings. Conf.c may
 * live only in mods; the declarations live here so core files can call it.
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

/* ---------------- core services ---------------- */

void retune_timer(void);
const struct led_mode *mode_owns(const char *pkg);
long eval_and_write(void);
void apply_charge_leds(void);
void arm_notification(struct notif_state *st, const char *pkg);
void disarm_notification(struct notif_state *st, const char *why);
void handle_enqueue(struct notif_state *st, const char *pkg);
void ev_load_tags(void);
int  ev_parse(const unsigned char *buf, size_t n, char *pkg, size_t pkgn);

/* ---------------- extension entry points ---------------- */

int  ring_is_active(void);          /* ring mode armed right now */
void arm_ring(int incoming);        /* 1 = incoming, 0 = outgoing */
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
        { (_name), (_tick_ms), (_owns), (_tick) }

/* section bounds, synthesized by the linker around the registry data */
extern const struct led_rule        __start_chgd_rules[];
extern const struct led_rule        __stop_chgd_rules[];
extern const struct pkg_handler     __start_chgd_handlers[];
extern const struct pkg_handler     __stop_chgd_handlers[];
extern const struct led_mode        __start_chgd_modes[];
extern const struct led_mode        __stop_chgd_modes[];

#endif /* CHGD_H */