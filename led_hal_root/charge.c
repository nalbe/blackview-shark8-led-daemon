/*
 * charge.c - charge band evaluation and LED application.
 * band is recomputed on battery uevents and written to the state file;
 * the LEDs are rewritten only on real state changes (g_applied_band
 * fingerprint: band + colors + light type + timings).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "chgd.h"

#define STATE_PATH "/data/local/tmp/led_chg"
#define STATE_TMP  "/data/local/tmp/led_chg.tmp"

/* charge thresholds / breath timing defaults live in mods/conf.c
 * ([charge] section of led.conf). Builtin fallbacks are defined there too;
 * these getters wrap the runtime config. */

/* last applied charge state: band + color + light type + timings.
 * Rewritten only on a real change, otherwise every tick/uevent would
 * blink the LED off->on. Invalidation: arm_notification / arm_ring set
 * the first byte to NUL, forcing a reapply on the next charge pass. */
char g_applied_band[64] = "\x01INIT";

static const char *band_for(const char *status, const char *cap)
{
    int c = -1;
    if (cap && *cap) {
        c = atoi(cap);
        for (const char *p = cap; *p; p++)
            if (*p < '0' || *p > '9') { c = -1; break; }
    }

    /* range names are abstract (lower/middle/upper); the actual colors
     * and light types per range come from led.conf, not hardcoded */
    if (!strcmp(status, "Full"))
        return "upper";

    if (!strcmp(status, "Charging")) {
        int second = conf_second_threshold();
        int first = conf_first_threshold();
        if (c >= second) return "upper";
        if (c >= first) return "middle";
        return "lower";
    }

    if (!strcmp(status, "Not charging"))
        return (c >= conf_second_threshold()) ? "upper" : "none";

    return "none";
}

long eval_and_write(void)
{
    char status[64] = "", cap[16] = "";
    read_line("/sys/class/power_supply/battery/status", status, sizeof(status));
    read_line("/sys/class/power_supply/battery/capacity", cap, sizeof(cap));

    const char *band = band_for(status, cap);
    long ts = (long)time(NULL);

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s %ld\n", band, ts);

    int fd = open(STATE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ssize_t ig = write(fd, buf, (size_t)len); (void)ig;
        close(fd);
        rename(STATE_TMP, STATE_PATH);
    }
    return ts;
}

void apply_charge_leds(void)
{
    char line[64] = "";
    read_line(STATE_PATH, line, sizeof(line));
    char *sp = strchr(line, ' ');
    if (sp) *sp = '\0';
    const char *band = line;

    int cr, ch, cf, ct, r = 0, g = 0, b = 0, type = 0;
    conf_charge_timing(&cr, &ch, &cf, &ct);
    if (!strcmp(band, "lower")) {
        conf_charge_lower_rgb(&r, &g, &b);
        type = conf_charge_lower_type();
    } else if (!strcmp(band, "middle")) {
        conf_charge_middle_rgb(&r, &g, &b);
        type = conf_charge_middle_type();
    } else if (!strcmp(band, "upper")) {
        conf_charge_upper_rgb(&r, &g, &b);
        type = conf_charge_upper_type();
    }

    char fp[64];
    snprintf(fp, sizeof(fp), "%s %d,%d,%d t%d %d/%d/%d/%d",
             band, r, g, b, type, cr, ch, cf, ct);
    if (!strcmp(g_applied_band, fp))
        return;

    /* per-range light type: 0=off 1=breathing 2=flashing 3=static */
    if (type == 0 || !strcmp(band, "none")) {
        leds_all_off();
    } else if (type == 1) {
        led_breathe_rgb(r, g, b, cr, ch, cf, ct);
    } else if (type == 2) {
        /* flashing: square blink, fade ramps forced to 0 */
        led_breathe_rgb(r, g, b, 0, ch, 0, ct);
    } else {
        led_solid_rgb(r, g, b);
    }
    snprintf(g_applied_band, sizeof(g_applied_band), "%s", fp);
    LOGI("charge leds -> %s (rgb=%d,%d,%d type=%d)", band, r, g, b, type);
}