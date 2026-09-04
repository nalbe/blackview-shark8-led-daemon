/*
 * util.c - logging, sysfs read/write helpers, live status file,
 * screen detection. Pure device plumbing; no policy here. Shared by the
 * core and mods. All LED-channel writes live in led.c, not here.
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

/* Runtime logging switch, driven by [led] logging in led.conf (config.c
 * calls log_set_enabled() every time the config is (re)loaded). When off,
 * log_line() becomes a no-op: no file write, no syscalls beyond the
 * caller's own work. The GUI's ledd.log (tail) card just shows whatever
 * was written before the switch left. */
static int g_log_allow = 1;

void log_set_enabled(int on)
{
    g_log_allow = on ? 1 : 0;
}

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
    conf_maybe_reload();            /* refresh the [led] logging switch cheaply */
    if (!g_log_allow) return;
    FILE *n = fopen("/data/local/tmp/ledd.log", "a");
    if (!n) return;
    long sz = ftell(n);
    if (sz > 65536) {
        long half = sz / 2;
        char *tmp = malloc(half);
        if (tmp) {
            size_t r = fread(tmp, 1, (size_t)half, n);
            if (r > 0) {
                fclose(n);
                n = fopen("/data/local/tmp/ledd.log", "w");
                if (n) { fwrite(tmp, 1, r, n); }
            }
            free(tmp);
        } else {
            fclose(n);
            n = fopen("/data/local/tmp/ledd.log", "w");
        }
        if (!n) return;
    }
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

/* Verbose LED plumbing log. Every sysfs write to the RGB / backlight
 * channels is recorded so a run can be reconstructed afterwards: exactly
 * what chgd told each channel, in what order. brightness writes are
 * logged too (the software breath repaints them every tick), each line
 * carries the full path so channel + attribute are unambiguous. */
static int led_path(const char *path, const char **ch)
{
    static const char *LEDS[] = { "red", "green", "blue", "lcd-backlight" };
    for (int i = 0; i < (int)(sizeof(LEDS) / sizeof(LEDS[0])); i++)
        if (strstr(path, LEDS[i])) { *ch = LEDS[i]; return 1; }
    return 0;
}

void write_sys(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (g_verbose) fprintf(stderr, "write %s: %s\n", path, strerror(errno));
        return;
    }
    /* Log LED plumbing, but skip plain /brightness writes: the software
     * breath repaints them every tick (100ms) and would flood the log.
     * The peak brightness set by led_set() is logged there instead. */
    const char *ch;
    if (led_path(path, &ch) && !strstr(path, "/brightness"))
        LOGI("LED %s <- %s", path, val);
    ssize_t ignored = write(fd, val, strlen(val));
    (void)ignored;
    close(fd);
}

/* ---------------- live status file ---------------- */

#define STATUS_PATH "/data/local/tmp/led_status"
#define STATUS_TMP  "/data/local/tmp/led_status.tmp"

/* Write the current LED-owner state for external consumers (GUI etc.).
 * mode: charge | notify | ring | voip
 * band: lower/middle/upper/none (charge only, "" otherwise)
 * pkg:  armed notification / ring / voip pseudo-package, "" when none
 * Atomic tmp + rename, same pattern as charge.c's led_chg. */
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, int type)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "ts=%ld\nmode=%s\nband=%s\npkg=%s\ncolor=%d,%d,%d\ntype=%d\n",
        (long)time(NULL), mode, band, pkg ? pkg : "", r, g, b, type);
    int fd = open(STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    ssize_t ig = write(fd, buf, (size_t)len); (void)ig;
    close(fd);
    rename(STATUS_TMP, STATUS_PATH);
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