/*
 * mods/notify.c - notification pipeline mod.
 *
 * Claims every notification the plugins did not take, and owns the
 * ARMED channel of the daemon:
 *   REGISTER_HANDLER("*", ...)   default handler: suppression list,
 *                                screen-on drop, dedup, then arm
 *   REGISTER_MODE("notify", ...) owns any armed real package while
 *                                ring/charge own their own channels;
 *                                heartbeat 1s -> timeout cap +
 *                                screen-on disarm from the tick
 * Registry lookups are link-time (core's pkg_dispatch walks exact
 * claiming handlers first, so dialer gets its shot before the "*"
 * default here); the pseudo-package colors (dialer's missed.call) come
 * from the link-time rules registry, per-app colors are runtime-only
 * (led.conf [rules]). The core never changes for a new app; the
 * events-buffer decode itself lives in the core (ev_parse/ev_load_tags
 * are transport, not policy).
 *
 * NOTE: suppression is enforced per event here, in the default path.
 * Exact-match plugin claims (dialer) bypass it by design - a plugin
 * owns its package's full response.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "../chgd.h"

/* heartbeat while a notification is armed (timeout cap + screen watch) */
#define NOTIFY_TICK_MS 1000
/* when a permanent (cap=0) led is up there is no periodic work: the
 * screen-on disarm comes from a netlink uevent, so we only need a rare
 * safety wakeup to catch a lost uevent - not a hard 1s poll. */
#define NOTIFY_PERM_SEC 60

/* ---------------- registry lookups ---------------- */

/* colour for a package: [rules] in led.conf, then internal pseudo-package
 * registry (missed.call), then the [notify] default_color */
static void rgb_for(const char *pkg, int *r, int *g, int *b)
{
    if (conf_pkg_rgb(pkg, r, g, b)) return;
    const struct led_rule *lr;
    for (lr = __start_chgd_rules; lr < __stop_chgd_rules; lr++)
        if (!strcmp(lr->pkg, pkg)) {
            *r = lr->r; *g = lr->g; *b = lr->b;
            return;
        }
    conf_notif_rgb(r, g, b);
}

/* suppressed = [suppress] in led.conf only (runtime, no link-time list) */
static int suppressed(const char *pkg)
{
    return conf_suppressed(pkg);
}

/* Rate-limit the log spam from persistent background emitters (e.g. VPN
 * foreground notifications fire every second): at most one suppressed
 * pair of lines per package per window, regardless of event frequency. */
#define SUPP_LOG_WINDOW_SEC 60
#define SUPP_LOG_SLOTS      32
static struct {
    char   pkg[128];
    time_t last;
} s_supp_log[SUPP_LOG_SLOTS];

static int supp_log_ok(const char *pkg)
{
    time_t now = time(NULL);
    for (int i = 0; i < SUPP_LOG_SLOTS; i++) {
        if (!s_supp_log[i].pkg[0]) {
            snprintf(s_supp_log[i].pkg, sizeof(s_supp_log[i].pkg), "%s", pkg);
            s_supp_log[i].last = now;
            return 1;
        }
        if (!strcmp(s_supp_log[i].pkg, pkg)) {
            if (now - s_supp_log[i].last < SUPP_LOG_WINDOW_SEC) return 0;
            s_supp_log[i].last = now;
            return 1;
        }
    }
    return 1;   /* table full: fall back to logging */
}

/* ---------------- arming / disarming ---------------- */

void arm_notification(struct notif_state *st, const char *pkg)
{
    arm_notification_ex(st, pkg, 0);
}

void arm_notification_ex(struct notif_state *st, const char *pkg, int test)
{
    int rise, hold, fall, offt, r, g, b;
    int type = conf_notif_type();
    conf_notif_timing(&rise, &hold, &fall, &offt);
    rgb_for(pkg, &r, &g, &b);
    g_applied_band[0] = '\0';       /* LEDs taken over: force reapply later */
    int soft = (int)conf_get_int("notify", "notify_soft_breath", 0);
    /* [notify] notify_light_type: 0=off 1=breathing 2=flashing 3=static */
    if (type == 0) {
        leds_all_off();
    } else if (type == 2) {
        led_breathe_rgb(r, g, b, 0, hold, 0, offt);
    } else if (type == 3) {
        led_solid_rgb(r, g, b);
    } else if (soft) {
        /* software breath: exact channel amplitudes (aw2033 hardware
         * breath would burn every active channel at full 255) */
        leds_all_off();
        led_soft_breathe_rgb(r, g, b,
                             conf_get_int("notify", "notify_soft_cycle_ms", 25500));
    } else {
        led_breathe_rgb(r, g, b, rise, hold, fall, offt);
    }
    snprintf(st->cur_pkg, sizeof(st->cur_pkg), "%s", pkg);
    st->armed_at = time(NULL);
    st->test = test;
    status_write("notify", "", pkg, r, g, b, type);
    retune_timer();
    LOGI("notify armed: %s rgb=%d,%d,%d type=%d%s%s", pkg, r, g, b, type,
         test ? " (test)" : "", soft ? " soft" : "");
}

void disarm_notification(struct notif_state *st, const char *why)
{
    if (!st->cur_pkg[0]) return;
    LOGI("notify disarmed (%s): %s", why, st->cur_pkg);
    st->cur_pkg[0] = '\0';
    st->test = 0;
    apply_charge_leds();            /* the idle owner takes over */
    retune_timer();
}

/* ---------------- default enqueue dispatch ---------------- */

/* Notifications usually arrive on a portrait look: the screen just woke
 * (peek/ambient) and is still on for a couple of seconds. Dropping on
 * screen-on loses the led flash entirely, so instead we park the event
 * here and paint it the moment the screen falls back asleep (uevent) or
 * the small grace window (notify_screen_delay_ms) expires. */
static char      g_pending_pkg[128];
static time_t    g_pending_at;

void notify_pending_check(void)
{
    if (!g_pending_pkg[0]) return;
    long    delay = conf_get_int("notify", "notify_screen_delay_ms", 60000);
    double  age = difftime(time(NULL), g_pending_at);
    if (screen_on())
        return;            /* still looking: keep it parked, flash on screen-off */
    char pkg[128];
    snprintf(pkg, sizeof(pkg), "%s", g_pending_pkg);
    if (age <= (double)delay / 1000.0) {
        LOGI("screen off: pending armed: %s", pkg);
        g_pending_pkg[0] = '\0';
        arm_notification(&g_st, pkg);
    } else {
        LOGI("screen off: pending dropped (stale %ds): %s", (int)age, pkg);
        g_pending_pkg[0] = '\0';
    }
}

static int notify_default_handle(const char *pkg)
{
    conf_maybe_reload();            /* pick up edited led.conf on the fly */
    if (voip_active()) {
        /* messenger call rainbow is the priority: any notification while it
         * runs is parked for later, never clobbers the rainbow */
        LOGI("voip in progress, ignore %s", pkg);
        return 1;
    }
    if (suppressed(pkg)) {
        if (supp_log_ok(pkg)) {
            LOGI("enqueue event: %s", pkg);
            LOGI("suppressed: %s", pkg);
        }
        return 1;
    }
    LOGI("enqueue event: %s", pkg);
    if (voip_try(pkg)) return 1;    /* messenger call -> rainbow; chat falls through */
    if (screen_on()) {
        /* screen is up for the peek: park it, flash on screen-off
         * (or let the grace window run out if the user keeps screen) */
        if (g_pending_pkg[0] && strcmp(g_pending_pkg, pkg))
            LOGI("pending replace: %s -> %s", g_pending_pkg, pkg);
        else if (!g_pending_pkg[0])
            LOGI("pending (screen on): %s", pkg);
        snprintf(g_pending_pkg, sizeof(g_pending_pkg), "%s", pkg);
        g_pending_at = time(NULL);
        return 1;
    }
    if (!strcmp(g_st.cur_pkg, pkg)) {
        LOGI("already armed: %s", pkg);
        return 1;
    }
    arm_notification(&g_st, pkg);
    return 1;
}

/* ---------------- armed mode ---------------- */

/* owns any armed real package; defers to ring on its pseudo-package and
 * to charge on the idle channel (""), so the three modes partition
 * g_st.cur_pkg exactly. */
static int notify_owns(const char *pkg)
{
    if (!pkg || !pkg[0]) return 0;      /* idle belongs to charge */
    if (ring_is_active()) return 0;     /* ring owns its pseudo-pkg */
    if (voip_active()) return 0;        /* voip rainbow owns its pseudo-pkg */
    return 1;
}

static void notify_tick(void)
{
    static int cnt;
    notify_pending_check();         /* belt & braces next to the uevent */
    if (++cnt % 10 == 0) LOGI("armed tick");
    double age = difftime(time(NULL), g_st.armed_at);
    long cap = conf_notif_max_sec();
    if (cap > 0 && age >= cap)          /* 0 = unlimited */
        disarm_notification(&g_st, "timeout");
    else if (!g_st.test && screen_on())
        disarm_notification(&g_st, "screen on (tick)");
}

/* Adaptive heartbeat: instead of a hard 1s poll while the led is up, return
 * until the next real piece of work so the core can sleep:
 *   a parked (screen-on grace) pending notify -> 1s, it is transient
 *   finite notif_max_sec cap                 -> wake exactly when it expires
 *   permanent led (cap=0)                    -> quiet, screen-on disarming is
 *                                               uevent-driven, rare safety only */
static long notify_next_wake(void)
{
    if (g_pending_pkg[0])
        return NOTIFY_TICK_MS;                 /* grace window: keep reactivity */
    long cap = conf_notif_max_sec();
    if (cap > 0) {
        double age = difftime(time(NULL), g_st.armed_at);
        long remain = (long)(cap - age);
        if (remain < 1) remain = 1;            /* cap due: tick promptly, disarm */
        return remain * 1000L;
    }
    return NOTIFY_PERM_SEC * 1000L;            /* permanent led: rare safety only */
}

/* fb0/lcd kernel uevents: disarm on screen-on, like the old core's
 * netlink branch used to do; flash a parked notification the moment
 * the screen goes back off. The ring rainbow ignores the screen (the
 * incoming-call UI wakes the display and must not kill the effect);
 * test hooks ignore it too (user is watching the app). */
static void notify_screen_uevent(void)
{
    if (g_st.cur_pkg[0] &&
        !ring_is_active() &&
        !voip_active() &&
        !g_st.test &&
        screen_on())
        disarm_notification(&g_st, "screen on");
    if (!screen_on())
        notify_pending_check();     /* screen just fell: paint it now */
}

REGISTER_HANDLER("*", notify_default_handle);
REGISTER_UEVENT("fb0", notify_screen_uevent);
REGISTER_UEVENT("lcd", notify_screen_uevent);
REGISTER_MODE_WAKE("notify", NOTIFY_TICK_MS, notify_owns, notify_tick, notify_next_wake);