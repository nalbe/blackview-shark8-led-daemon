/*
 * util.c - logging, sysfs LED primitives, screen detection.
 * Pure device plumbing; no policy here. Shared by the core and mods.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "chgd.h"

int g_verbose = 0;

void log_line(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_verbose) {
        fprintf(stderr, "%s\n", msg);
        return;
    }
    FILE *n = fopen("/data/local/tmp/ledd.log", "a");
    if (!n) return;
    time_t t = time(NULL);
    struct tm tm_;
    char tbuf[32];
    localtime_r(&t, &tm_);
    strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M:%S", &tm_);
    fprintf(n, "%s %s\n", tbuf, msg);
    fclose(n);
}

/* ---------------- sysfs helpers ---------------- */

int read_line(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

void write_sys(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (g_verbose) fprintf(stderr, "write %s: %s\n", path, strerror(errno));
        return;
    }
    ssize_t ignored = write(fd, val, strlen(val));
    (void)ignored;
    close(fd);
}

void led_set(const char *c, int level)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/class/leds/%s/trigger", c);
    write_sys(p, "none");
    snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", c);
    char v[8]; snprintf(v, sizeof(v), "%d", level);
    write_sys(p, v);
}

/* steady RGB: per-channel brightness only; blink/led_time already cleared
 * by leds_all_off() (stale led_time keeps pulsing otherwise) */
void led_solid_rgb(int r, int g, int b)
{
    static const char *leds[] = { "red", "green", "blue" };
    int vals[3] = { r, g, b };
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
    leds_all_off();
    for (int i = 0; i < 3; i++) {
        if (vals[i] <= 0) continue;
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
    static const char *leds[] = { "red", "green", "blue" };
    for (int i = 0; i < 3; i++) {
        char p[128];
        snprintf(p, sizeof(p), "/sys/class/leds/%s/blink", leds[i]);
        write_sys(p, "0");
        snprintf(p, sizeof(p), "/sys/class/leds/%s/led_time", leds[i]);
        write_sys(p, "0 0 0 0");
        led_set(leds[i], 0);
    }
}

/* raw per-channel brightness write; used by the rainbow cycler */
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

/* ---------------- screen state ---------------- */

/* returns 1 = screen on, 0 = off */
int screen_on(void)
{
    /* primary: lcd backlight level */
    char buf[32];
    if (read_line("/sys/class/leds/lcd-backlight/brightness",
                  buf, sizeof(buf)) == 0) {
        return atoi(buf) > 0;
    }
    /* fallback: fb blank node, "0" = unblanked/on */
    if (read_line("/sys/class/graphics/fb0/blank", buf, sizeof(buf)) == 0) {
        return strcmp(buf, "0") == 0;
    }
    return 0;                    /* unknown -> assume off, show notification */
}