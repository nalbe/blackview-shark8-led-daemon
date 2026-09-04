/*
 * led.c - everything that actually touches the LED hardware.
 *
 * Single choke point for the three RGB channels: solid / breathing /
 * flashing writes plus the timer-driven rainbow cycler. Every other
 * module (core, charge, notify, ring) lights the LEDs ONLY through the
 * exported led_* calls in chgd.h - never by poking sysfs directly.
 *
 * The rainbow is a segment cross-fade cycler: 8 palette anchors, 16
 * eased steps per segment, ~4.1s full cycle at the 32ms mode heartbeat.
 * ring.c only starts/stops it (led_rainbow_reset/step/rgb); the palette
 * and the easing live here and nowhere else.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "chgd.h"

/* ---------------- per-channel sysfs plumbing ---------------- */

void led_set(const char *c, int level)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/class/leds/%s/trigger", c);
    write_sys(p, "none");
    snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", c);
    char v[8]; snprintf(v, sizeof(v), "%d", level);
    LOGI("LED peak %s <- %s", p, v);
    write_sys(p, v);
}

/* steady RGB: per-channel brightness only; blink/led_time already cleared
 * by leds_all_off() (stale led_time keeps pulsing otherwise) */
void led_solid_rgb(int r, int g, int b)
{
    static const char *leds[] = { "red", "green", "blue" };
    int vals[3] = { r, g, b };
    LOGI("[led] solid rgb=%d,%d,%d", r, g, b);
    leds_all_off();
    for (int i = 0; i < 3; i++) {
        if (vals[i] <= 0) continue;
        char p[128], v[8];
        snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", leds[i]);
        snprintf(v, sizeof(v), "%d", vals[i]);
        write_sys(p, v);
    }
}

/* breathing RGB: per-channel brightness is the breath peak, timing from
 * led_time "rise hold fall offt" (ms, quantized by aw2033 driver) */
void led_breathe_rgb(int r, int g, int b, int rise, int hold, int fall, int offt)
{
    static const char *leds[] = { "red", "green", "blue" };
    int vals[3] = { r, g, b };
    LOGI("[led] breathe rgb=%d,%d,%d t=%d,%d,%d,%dms (h/w blink)", r, g, b,
         rise, hold, fall, offt);
    leds_all_off();
    for (int i = 0; i < 3; i++) {
        if (vals[i] <= 0) { LOGI("[led]   skip %s (amp 0)", leds[i]); continue; }
        char p[128], v[64];
        led_set(leds[i], vals[i]);
        snprintf(p, sizeof(p), "/sys/class/leds/%s/led_time", leds[i]);
        snprintf(v, sizeof(v), "%d %d %d %d", rise, hold, fall, offt);
        write_sys(p, v);
        snprintf(p, sizeof(p), "/sys/class/leds/%s/blink", leds[i]);
        write_sys(p, "1");
    }
}

void leds_all_off(void)
{
    led_soft_breathe_stop();
    static const char *leds[] = { "red", "green", "blue" };
    LOGI("[led] all off");
    for (int i = 0; i < 3; i++) {
        char p[128];
        snprintf(p, sizeof(p), "/sys/class/leds/%s/blink", leds[i]);
        write_sys(p, "0");
        snprintf(p, sizeof(p), "/sys/class/leds/%s/led_time", leds[i]);
        write_sys(p, "0 0 0 0");
        led_set(leds[i], 0);
    }
}

/* raw per-channel brightness write; used by the rainbow cycler below */
void set_rgb(int r, int g, int b)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", r);
    write_sys("/sys/class/leds/red/brightness", v);
    snprintf(v, sizeof(v), "%d", g);
    write_sys("/sys/class/leds/green/brightness", v);
    snprintf(v, sizeof(v), "%d", b);
    write_sys("/sys/class/leds/blue/brightness", v);
}

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
static int g_cur_r, g_cur_g, g_cur_b;   /* last rainbow color, for status */

void led_rainbow_reset(void)
{
    g_rb_step = 0;
}

void led_rainbow_rgb(int *r, int *g, int *b)
{
    *r = g_cur_r; *g = g_cur_g; *b = g_cur_b;
}

void led_rainbow_step(void)
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
    g_cur_r = r; g_cur_g = g; g_cur_b = bl;
    if (++g_rb_step >= RB_TOTAL) g_rb_step = 0;
}

/* ---------------- software breathing ---------------- */

/* Software breathing. The aw2033 hardware breath ignores the channel
 * amplitude (any channel with led_time+blink pulses 0..255), so dim
 * colors go white. This engine reproduces breathing in software - a
 * dedicated thread writes the eased phase straight to brightness every
 * tick, keeping each r,g,b amplitude exact. Stopped by leds_all_off()
 * and restarted on demand. */

/* Linear ramp (triangle wave): constant speed up and down, no dwelling
 * at either end. Rise and fall always share one ramp each, with no hold
 * and no off - the cycle is 0..255..0 and the led goes fully dark
 * between breaths. */
static int soft_phase(long x, long rise, long fall)
{
    long T = rise + fall;
    if (T <= 0) return 255;
    long p = x % T;
    if (rise > 0 && p < rise)
        return (int)(p * 255 / rise);            /* up / rise   */
    if (fall > 0)
        return 255 - (int)((p - rise) * 255 / fall); /* down / fall */
    return 255;
}

static volatile int g_soft_run;
static volatile int g_soft_gen;        /* kills stale threads on reapply */
static int g_soft_r, g_soft_g, g_soft_b;
static long g_soft_ms;
static int  g_soft_rise, g_soft_fall;   /* ramp durations, ms */
static int  g_soft_tick;                /* write period, ms */
static int  g_last[3];                  /* currently applied channel values */
static int  g_acc[3];                   /* fractional step accumulators, x1000 */

static pthread_t g_soft_tid;
static int g_soft_started;

static void *soft_loop(void *arg)
{
    (void)arg;
    /* generation pin: a stale thread from a previous apply must die even
     * if a newer apply already set g_soft_run back to 1, or every GUI
     * save would pile up one more permanent thread feeding g_soft_ms and
     * the cycle would run N times too fast. */
    int mygen = g_soft_gen;
    while (g_soft_run && mygen == g_soft_gen) {
        g_soft_ms += g_soft_tick;
        usleep(g_soft_tick * 1000);
        /* re-check after the sleep: stop() may have fired while we were
         * napping. Without this the thread paints one last phase value
         * over a hardware breath apply and drags its peak (brightness)
         * down to a dim fraction - the "black" test bands. */
        if (!g_soft_run || mygen != g_soft_gen)
            break;
        int v = soft_phase(g_soft_ms, g_soft_rise, g_soft_fall);
        int tgt[3] = {
            g_soft_r ? g_soft_r * v / 255 : 0,
            g_soft_g ? g_soft_g * v / 255 : 0,
            g_soft_b ? g_soft_b * v / 255 : 0
        };
        /* fractional accumulator: a channel steps by 1 only when its
         * share of the tick has piled up to a whole unit, so partial
         * channels move at their natural pace, not at the max one */
        for (int i = 0; i < 3; i++) {
            g_acc[i] += (tgt[i] - g_last[i]) * 1000;
            while (g_acc[i] >= 1000) { g_last[i]++; g_acc[i] -= 1000; }
            while (g_acc[i] <= -1000) { g_last[i]--; g_acc[i] += 1000; }
        }
        set_rgb(g_last[0], g_last[1], g_last[2]);
    }
    return NULL;
}

void led_soft_breathe_rgb(int r, int g, int b, long phase_ms)
{
    /* Fixed cycle shape - config timings are ignored on purpose:
     * one ramp = phase_ms (e.g. 25500 = 255 pwm steps x 100ms at
     * max channel 255), then the same ramp down, no hold, no off
     * pause. Always. Dimmer channels step through fractional
     * accumulators at their own pace. */
    if (phase_ms < 50) phase_ms = 50;
    int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
    long tick = mx > 0 ? phase_ms / mx : 50;
    if (tick < 50) tick = 50;               /* max channel 255 -> 100ms at 25500 */
    g_soft_gen++;                    /* invalidate any stale thread */
    g_soft_r = r; g_soft_g = g; g_soft_b = b;
    g_soft_rise = (int)phase_ms;
    g_soft_fall = (int)phase_ms;
    g_soft_ms = 0;
    g_soft_tick = (int)tick;
    g_last[0] = g_last[1] = g_last[2] = 0;
    g_acc[0] = g_acc[1] = g_acc[2] = 0;
    LOGI("soft breath: r=%d g=%d b=%d cycle=%ld+%ldms tick=%ldms",
         r, g, b, phase_ms, phase_ms, tick);
    g_soft_run = 1;
    if (pthread_create(&g_soft_tid, NULL, soft_loop, NULL) == 0) {
        g_soft_started = 1;
    } else {
        g_soft_started = 0;
        g_soft_run = 0;         /* threadless: nothing will ever paint */
    }
}

void led_soft_breathe_stop(void)
{
    g_soft_run = 0;
    g_soft_gen++;
    if (g_soft_started) {
        /* wait for the thread to actually finish: it may be mid-sleep
         * and would otherwise wake up and paint one last phase value
         * over whatever the caller applies next (hardware breath peak
         * gets dragged down to a dim fraction -> the "black" bands) */
        pthread_join(g_soft_tid, NULL);
        g_soft_started = 0;
    }
}