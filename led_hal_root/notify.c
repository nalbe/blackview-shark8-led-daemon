/*
 * notify.c - notification pipeline:
 *   1. parses raw events-buffer packets into a package name
 *   2. dispatches enqueue events to plugins / color rules
 *   3. arms/disarms the hardware breathing
 *
 * Registry lookups are link-time: chgd_rules carries only internal
 * pseudo-package colors (dialer's missed.call); claiming handlers come
 * from any mod via REGISTER_HANDLER. The core never changes for a new
 * app. Per-app colors and suppression are runtime-only (led.conf).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "chgd.h"

#define TAGS_FILE    "/system/etc/event-log-tags"
#define TAG_FALLBACK 27501            /* AOSP notification_enqueue */

/* notification breathing timing (ms) - default; [notify] in led.conf wins */
#define RISE 500
#define HOLD 100
#define FALL 500
#define OFFT 1200

struct notif_state g_st;               /* single instance, shared universe */

/* ---------------- events-buffer parsing ---------------- */

static unsigned g_tag_enq = TAG_FALLBACK;

void ev_load_tags(void)
{
    FILE *f = fopen(TAGS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int id;
        char name[128];
        if (sscanf(line, "%d %127s", &id, name) == 2 &&
            !strcmp(name, "notification_enqueue")) {
            g_tag_enq = (unsigned)id;
            break;
        }
    }
    fclose(f);
    LOGI("notification_enqueue tag id = %u", g_tag_enq);
}

/*
 * Raw events entry: [logger_entry hdr][payload]
 *   payload: int32 tag_id (BE), then binary value list:
 *     byte type; INT(1)=int32 BE, STRING(3)=int32 len BE + bytes,
 *     LIST(4)=int32 content_len BE + nested values
 * enqueue fields: (uid int)(pid int)(pkg string)...
 * Returns: 1 and fills pkg when this is our tag, else 0.
 */
/* Wire format on this device (verified by raw dumps, little-endian).
 * Strict positional walking proved unreliable across tags (enqueue carries
 * an extra zero byte vs other tags), so after matching the tag id we locate
 * the package name by anchoring on its self-describing string record:
 *   [0x02][u32le len][len bytes of [a-zA-Z0-9._] with >=1 dot][NUL]
 * which uniquely identifies the pkg field of notification_enqueue. */
static unsigned le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static int extract_pkg(const unsigned char *p, size_t n,
                       char *out, size_t on)
{
    for (size_t i = 0; i + 6 <= n; i++) {
        if (p[i] != 0x02) continue;
        unsigned len = le32(p + i + 1);
        if (len < 3 || len > 64 || i + 5 + len + 1 > n) continue;
        const unsigned char *s = p + i + 5;
        if (s[len] != 0x00) continue;          /* records carry a trailing NUL */
        if (s[0] < 'a' || s[0] > 'z') continue;
        int dots = 0, ok = 1;
        for (unsigned j = 0; j < len; j++) {
            unsigned char c = s[j];
            int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '_';
            if (!alnum) { ok = 0; break; }
            if (c == '.') dots++;
        }
        if (!ok || dots < 1) continue;
        size_t c = len < on - 1 ? len : on - 1;
        memcpy(out, s, c);
        out[c] = '\0';
        return 1;
    }
    return 0;
}

int ev_parse(const unsigned char *buf, size_t n,
             char *pkg, size_t pkgn)
{
    if (n < 8) return 0;
    unsigned hdr = buf[2] | (buf[3] << 8);
    if (hdr < 8 || hdr >= n) hdr = 28;

    const unsigned char *p = buf + hdr;
    size_t left = n - hdr;

    if (left < 6) return 0;
    unsigned tag = le32(p);
    if (tag != g_tag_enq) return 0;

    return extract_pkg(p + 4, left - 4, pkg, pkgn);
}

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

/* ---------------- arming / disarming ---------------- */

void arm_notification(struct notif_state *st, const char *pkg)
{
    int rise, hold, fall, offt, r, g, b;
    int type = conf_notif_type();
    conf_notif_timing(&rise, &hold, &fall, &offt);
    rgb_for(pkg, &r, &g, &b);
    g_applied_band[0] = '\0';       /* LEDs taken over: force reapply later */
    /* [notify] notify_light_type: 0=off 1=breathing 2=flashing 3=static */
    if (type == 0) {
        leds_all_off();
    } else if (type == 2) {
        led_breathe_rgb(r, g, b, 0, hold, 0, offt);
    } else if (type == 3) {
        led_solid_rgb(r, g, b);
    } else {
        led_breathe_rgb(r, g, b, rise, hold, fall, offt);
    }
    snprintf(st->cur_pkg, sizeof(st->cur_pkg), "%s", pkg);
    st->armed_at = time(NULL);
    retune_timer();
    LOGI("notify armed: %s rgb=%d,%d,%d type=%d", pkg, r, g, b, type);
}

void disarm_notification(struct notif_state *st, const char *why)
{
    if (!st->cur_pkg[0]) return;
    LOGI("notify disarmed (%s): %s", why, st->cur_pkg);
    st->cur_pkg[0] = '\0';
    apply_charge_leds();
    retune_timer();
}

/* ---------------- enqueue dispatch ---------------- */

void handle_enqueue(struct notif_state *st, const char *pkg)
{
    conf_maybe_reload();            /* pick up edited led.conf on the fly */
    LOGI("enqueue event: %s", pkg);
    if (suppressed(pkg)) {
        LOGI("suppressed: %s", pkg);
        return;
    }
    /* claiming handlers run before the generic path (e.g. dialer) */
    const struct pkg_handler *h;
    for (h = __start_chgd_handlers; h < __stop_chgd_handlers; h++) {
        if (!strcmp(h->pkg, pkg) && h->fn(pkg)) {
            LOGI("claimed by handler: %s", pkg);
            return;
        }
    }
    if (screen_on()) {
        LOGI("dropped (screen on): %s", pkg);
        return;
    }
    if (!strcmp(st->cur_pkg, pkg)) {
        LOGI("already armed: %s", pkg);
        return;
    }
    arm_notification(st, pkg);
}