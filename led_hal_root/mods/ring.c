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

/* ---------------- rainbow cycler ---------------- */

/* Palette anchors. Between two anchors the color is cross-faded over
 * RB_SUB ticks with a cosine-eased curve, so hues glide instead of
 * snapping. Full cycle: 8 anchors * 16 steps * 32 ms ~= 4.1 s. */
static const unsigned char RAINBOW[][3] = {
    {255,   0,   0},   /* red      */
    {255,  96,   0},   /* orange   */
    {255, 255,   0},   /* yellow   */
    {  0, 255,   0},   /* green    */
    {  0, 255, 255},   /* cyan     */
    {  0,  80, 255},   /* azure    */
    {160,   0, 255},   /* violet   */
    {255,   0, 192},   /* magenta  */
};
#define RAINBOW_N (sizeof(RAINBOW) / sizeof(RAINBOW[0]))

#define RB_SUB   16                        /* interpolated steps / segment */
#define RB_TOTAL (RAINBOW_N * RB_SUB)
/* (1 - cos(pi * i / RB_SUB)) / 2 * 256, i = 0..RB_SUB */
static const unsigned short RB_EASE[RB_SUB + 1] = {
    0, 2, 10, 22, 37, 57, 79, 103, 128,
    153, 177, 199, 219, 234, 246, 254, 256
};

static size_t g_rb_step;

static void ring_step(void)
{
    size_t seg = (g_rb_step / RB_SUB) % RAINBOW_N;
    size_t nxt = (seg + 1) % RAINBOW_N;
    unsigned e = RB_EASE[g_rb_step % RB_SUB];
    const unsigned char *a = RAINBOW[seg];
    const unsigned char *b = RAINBOW[nxt];
    int r = a[0] + ((((int)b[0] - (int)a[0]) * (int)e) >> 8);
    int g = a[1] + ((((int)b[1] - (int)a[1]) * (int)e) >> 8);
    int bl = a[2] + ((((int)b[2] - (int)a[2]) * (int)e) >> 8);
    set_rgb(r, g, bl);
    if (++g_rb_step >= RB_TOTAL) g_rb_step = 0;
}

/* ---------------- mode state ---------------- */

static time_t g_last_tele_chk;
/* 1 if the current rainbow was caused by an incoming (RINGING) call;
 * 0 for an outgoing (OFFHOOK) call. On end, incoming resolves to a missed
 * call check, outgoing just drops back to the charge leds. */
static int g_ring_incoming;

int ring_is_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, INCOMING_PKG);
}

void arm_ring(int incoming)
{
    g_applied_band[0] = '\0';       /* charge leds must reapply after */
    leds_all_off();                 /* kill breathing, all channels 0 */
    g_rb_step = 0;
    g_ring_incoming = incoming;
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", INCOMING_PKG);
    g_st.armed_at = time(NULL);
    g_last_tele_chk = g_st.armed_at;
    ring_step();                    /* first color immediately */
    retune_timer();
    LOGI("ring armed -> rainbow (%s)",
         incoming ? "incoming" : "outgoing");
}

/* ---------------- mode tick ---------------- */

static int ring_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, INCOMING_PKG);
}

static void ring_tick(void)
{
    ring_step();
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
    double age = difftime(time(NULL), g_st.armed_at);
    if (age >= RING_MAX_SEC)
        disarm_notification(&g_st, "timeout");
}

REGISTER_MODE("ring", RING_STEP_MS, ring_owns, ring_tick);