/*
 * mods/ring.c - incoming/outgoing call rainbow mode.
 *
 * Entered when a dialer ping coincides with an active call. Registers a
 * timer MODE: while the pseudo-package is armed, the core drops the
 * timer to our heartbeat (RING_STEP_MS) and calls ring_tick() on every
 * tick. Ring mode deliberately ignores the screen state (this ROM wakes
 * the display for incoming calls and the screen-on guard would kill the
 * effect); it exits on call end, timeout, or explicit disarm.
 *
 * The rainbow itself lives in led.c: ring.c only resets / steps it and
 * reads back the current color for the status file. No LED code here.
 *
 * Config: [ring] test_sec=<n> in led.conf overrides the test-rainbow hold
 * (default RING_TEST_SEC). Looked up through config.c's generic kv store.
 *
 * The mode pattern is the extension seam for any LED "behavior" that is
 * driven by the timer rather than by a single arm/disarm:
 *   REGISTER_MODE("name", heartbeat_ms, owns_pkg_fn, tick_fn);
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define INCOMING_PKG   "incoming.call"   /* pseudo-pkg: ring rainbow */
#define RING_STEP_MS   32        /* fade tick: 128 ticks ~= 4.1s cycle */
#define RING_CHK_SEC   1         /* telephony state re-poll period */
#define RING_MAX_SEC   120       /* hard cap on rainbow even if state sticks */
#define RING_TEST_SEC  30        /* default test-rainbow hold ([ring] test_sec) */

/* test-rainbow hold, seconds; overridable via [ring] test_sec in led.conf */
static long ring_test_hold(void)
{
    long v = conf_get_int("ring", "test_sec", RING_TEST_SEC);
    return v > 0 ? v : RING_TEST_SEC;
}

/* ---------------- mode state ---------------- */

static time_t g_last_tele_chk;
/* 1 if the current rainbow was caused by an incoming (RINGING) call;
 * 0 for an outgoing (OFFHOOK) call. On end, incoming resolves to a missed
 * call check, outgoing just drops back to the charge leds. */
static int g_ring_incoming;
/* 1 when armed from a test hook: telephony state is ignored so the
 * rainbow is not disarmed by "call ended" a second after arming.
 * Held for RING_TEST_SEC, or until Disarm / USR2. */
static int g_ring_test;

int ring_is_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, INCOMING_PKG);
}

void arm_ring_ex(int incoming, int test)
{
    g_applied_band[0] = '\0';       /* charge leds must reapply after */
    leds_all_off();                 /* kill breathing, all channels 0 */
    led_rainbow_reset();
    g_ring_incoming = incoming;
    g_ring_test = test;
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", INCOMING_PKG);
    g_st.armed_at = time(NULL);
    g_last_tele_chk = g_st.armed_at;
    led_rainbow_step();             /* first color immediately */
    int cr, cg, cb;
    led_rainbow_rgb(&cr, &cg, &cb);
    status_write("ring", "",
                 incoming ? "incoming.call" : "outgoing.call",
                 cr, cg, cb, 0);
    retune_timer();
    LOGI("ring armed -> rainbow (%s%s)",
         incoming ? "incoming" : "outgoing",
         test ? ", test" : "");
}

void arm_ring(int incoming)
{
    arm_ring_ex(incoming, 0);
}

/* ---------------- mode tick ---------------- */

static int ring_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, INCOMING_PKG);
}

static void ring_tick(void)
{
    led_rainbow_step();
    if (!g_ring_test) {
        double dt = difftime(time(NULL), g_last_tele_chk);
        if (dt >= RING_CHK_SEC) {
            g_last_tele_chk = time(NULL);
            if (!tele_active()) {
                LOGI("call ended -> resolving outcome");
                g_st.cur_pkg[0] = '\0';
                apply_charge_leds();
                if (g_ring_incoming) {
                    /* keep verifying: the missed row may land now */
                    dialer_reopen_window();
                }
            }
        }
    }
    double age = difftime(time(NULL), g_st.armed_at);
    double cap = g_ring_test ? (double)ring_test_hold() : (double)RING_MAX_SEC;
    if (age >= cap)
        disarm_notification(&g_st, g_ring_test ? "test timeout" : "timeout");
}

REGISTER_MODE("ring", RING_STEP_MS, ring_owns, ring_tick);