/*
 * mods/conf.c - runtime INI config for the LED policies.
 *
 * Reads /data/adb/modules/led_hal_root/led.conf at runtime and merges it
 * over the link-time registry (internal pseudo-packages only, e.g.
 * dialer's missed.call) so the
 * user can tweak blacklists / colours / timings WITHOUT recompiling.
 *
 * SECTIONS:
 *   [suppress]  one package per line - never lights the LED
 *   [rules]     pkg=r,g,b   (0-255 per channel; no more bit masks)
 *   [charge]    first_threshold / second_threshold (%), per-range light
 *               type (0=off 1=breathing 2=flashing 3=static) and color
 *               r,g,b as lower/middle/upper_range_*,
 *               rise/hold/fall/offt (ms breath)
 *   [notify]    default_color as r,g,b, notify_light_type
 *               (0=off 1=breathing 2=flashing 3=static),
 *               rise/hold/fall/offt (ms breath), notif_max_sec (0 = unlimited)
 *
 * The file is reloaded lazily: every lookup calls conf_maybe_reload(),
 * which stat()s the file and only re-reads when its mtime changed. No I/O
 * happens in the steady loop unless the file was actually edited.
 *
 * Registry merge rules:
 *   - suppress list = file [suppress] entries ONLY (runtime; no link-time
 *     blacklist anymore - suppress.c was removed)
 *   - rules        = file [rules] entries override the link-time registry,
 *     which carries ONLY internal pseudo-packages (dialer's missed.call)
 *   - charge/notify colours+timings come from the file, falling back to
 *     builtins
 *
 * All config text and this file are ASCII-only (no non-ASCII in artifacts).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include "../chgd.h"

#define CONF_PATH "/data/adb/modules/led_hal_root/led.conf"
#define MAX_SUPP  96
#define MAX_RULES 96

/* builtin fallbacks (keep in sync with charge.c / notify.c defaults) */
#define DEF_FIRST_AT 90
#define DEF_SECOND_AT 95
#define DEF_C_RISE  700
#define DEF_C_HOLD  100
#define DEF_C_FALL  700
#define DEF_C_OFFT  900
#define DEF_N_RISE  500
#define DEF_N_HOLD  100
#define DEF_N_FALL  500
#define DEF_N_OFFT  1200
#define DEF_NOTIF_MAX 1800
/* builtin band behaviour: lower=breathing red, middle=flashing lime,
 * upper=static green, notif=breathing white */
#define DEF_LOWER_TYPE  1
#define DEF_MIDDLE_TYPE 2
#define DEF_UPPER_TYPE  3
#define DEF_LOWER_R 255
#define DEF_LOWER_G 0
#define DEF_LOWER_B 0
#define DEF_MIDDLE_R 96
#define DEF_MIDDLE_G 255
#define DEF_MIDDLE_B 0
#define DEF_UPPER_R 0
#define DEF_UPPER_G 255
#define DEF_UPPER_B 0
#define DEF_NTF_R 255
#define DEF_NTF_G 255
#define DEF_NTF_B 255
#define DEF_NTF_TYPE 1            /* notif light type: breathing */

static char    g_supp[MAX_SUPP][96];
static int     g_nsupp = 0;
static struct rule { char pkg[96]; int r, g, b; } g_rules[MAX_RULES];
static int     g_nrules = 0;

static int  g_first_at = DEF_FIRST_AT;
static int  g_second_at = DEF_SECOND_AT;
static int  g_lower_type = DEF_LOWER_TYPE;
static int  g_middle_type = DEF_MIDDLE_TYPE;
static int  g_upper_type = DEF_UPPER_TYPE;
static int  g_lower_r = DEF_LOWER_R, g_lower_g = DEF_LOWER_G, g_lower_b = DEF_LOWER_B;
static int  g_middle_r = DEF_MIDDLE_R, g_middle_g = DEF_MIDDLE_G, g_middle_b = DEF_MIDDLE_B;
static int  g_upper_r = DEF_UPPER_R, g_upper_g = DEF_UPPER_G, g_upper_b = DEF_UPPER_B;
static int  g_ntf_r = DEF_NTF_R, g_ntf_g = DEF_NTF_G, g_ntf_b = DEF_NTF_B;
static int  g_ntf_type = DEF_NTF_TYPE;
static int  g_c_rise = DEF_C_RISE, g_c_hold = DEF_C_HOLD;
static int  g_c_fall = DEF_C_FALL, g_c_offt = DEF_C_OFFT;
static int  g_n_rise = DEF_N_RISE, g_n_hold = DEF_N_HOLD;
static int  g_n_fall = DEF_N_FALL, g_n_offt = DEF_N_OFFT;
static long g_notif_max = DEF_NOTIF_MAX;

static time_t g_last_mtime = 0;

/* ---------------- helpers ---------------- */

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

static int in_supp(const char *pkg)
{
    for (int i = 0; i < g_nsupp; i++)
        if (!strcmp(g_supp[i], pkg)) return 1;
    return 0;
}

static int rule_rgb(const char *pkg, int *r, int *g, int *b)
{
    for (int i = 0; i < g_nrules; i++)
        if (!strcmp(g_rules[i].pkg, pkg)) {
            *r = g_rules[i].r; *g = g_rules[i].g; *b = g_rules[i].b;
            return 1;
        }
    return 0;
}

/* parse "r,g,b" with 0-255 per channel; returns 1 on success */
static int parse_rgb(const char *val, int *r, int *g, int *b)
{
    int x = 0, y = 0, z = 0;
    if (sscanf(val, "%d , %d , %d", &x, &y, &z) != 3) return 0;
    if (x < 0 || x > 255 || y < 0 || y > 255 || z < 0 || z > 255) return 0;
    *r = x; *g = y; *b = z;
    return 1;
}

/* parse light type 0..3 (0=off 1=breathing 2=flashing 3=static) */
static int parse_type(const char *val, int *t)
{
    int v = atoi(val);
    if (v < 0 || v > 3) return 0;
    *t = v;
    return 1;
}

/* ---------------- registry seeding ---------------- */

/* seed the rule table from the link-time registry. Only internal
 * pseudo-packages reside there (dialer's "missed.call"); real app
 * colors come from led.conf [rules] and win over these. */
static void seed_rules(void)
{
    const struct led_rule *r;
    int rr = 0, gg = 0, bb = 0;
    for (r = __start_chgd_rules; r < __stop_chgd_rules; r++) {
        if (g_nrules >= MAX_RULES) break;
        if (rule_rgb(r->pkg, &rr, &gg, &bb) == 0) {
            snprintf(g_rules[g_nrules].pkg, sizeof(g_rules[0].pkg), "%s", r->pkg);
            g_rules[g_nrules].r = r->r;
            g_rules[g_nrules].g = r->g;
            g_rules[g_nrules].b = r->b;
            g_nrules++;
        }
    }
}

/* ---------------- ---------------- */

static void reset_dynamic(void)
{
    g_nsupp = 0;
    g_nrules = 0;
    seed_rules();
    g_first_at = DEF_FIRST_AT;
    g_second_at = DEF_SECOND_AT;
    g_lower_type = DEF_LOWER_TYPE;
    g_middle_type = DEF_MIDDLE_TYPE;
    g_upper_type = DEF_UPPER_TYPE;
    g_lower_r = DEF_LOWER_R; g_lower_g = DEF_LOWER_G; g_lower_b = DEF_LOWER_B;
    g_middle_r = DEF_MIDDLE_R; g_middle_g = DEF_MIDDLE_G; g_middle_b = DEF_MIDDLE_B;
    g_upper_r = DEF_UPPER_R; g_upper_g = DEF_UPPER_G; g_upper_b = DEF_UPPER_B;
    g_ntf_r = DEF_NTF_R; g_ntf_g = DEF_NTF_G; g_ntf_b = DEF_NTF_B;
    g_ntf_type = DEF_NTF_TYPE;
    g_c_rise = DEF_C_RISE;  g_c_hold = DEF_C_HOLD;
    g_c_fall = DEF_C_FALL;  g_c_offt = DEF_C_OFFT;
    g_n_rise = DEF_N_RISE;  g_n_hold = DEF_N_HOLD;
    g_n_fall = DEF_N_FALL;  g_n_offt = DEF_N_OFFT;
    g_notif_max = DEF_NOTIF_MAX;
}

/* parse one key=value line into section */
static void parse_value(const char *sec, const char *key, const char *val)
{
    int r, g, b;
    if (!strcmp(sec, "charge")) {
        if      (!strcmp(key, "first_threshold")) {
            g_first_at = atoi(val);
            /* order-free: keep first <= second */
            if (g_first_at > g_second_at) {
                int t = g_first_at; g_first_at = g_second_at; g_second_at = t;
            }
        }
        else if (!strcmp(key, "second_threshold")) {
            g_second_at = atoi(val);
            if (g_first_at > g_second_at) {
                int t = g_first_at; g_first_at = g_second_at; g_second_at = t;
            }
        }
        else if (!strcmp(key, "lower_range_light_type")) {
            if (parse_type(val, &g_lower_type) == 0)
                LOGI("conf: bad [charge] lower_range_light_type=%s (0-3)", val);
        }
        else if (!strcmp(key, "middle_range_light_type")) {
            if (parse_type(val, &g_middle_type) == 0)
                LOGI("conf: bad [charge] middle_range_light_type=%s (0-3)", val);
        }
        else if (!strcmp(key, "upper_range_light_type")) {
            if (parse_type(val, &g_upper_type) == 0)
                LOGI("conf: bad [charge] upper_range_light_type=%s (0-3)", val);
        }
        else if (!strcmp(key, "lower_range_color")) {
            if (parse_rgb(val, &r, &g, &b)) {
                g_lower_r = r; g_lower_g = g; g_lower_b = b;
            } else {
                LOGI("conf: bad [charge] lower_range_color=%s (want r,g,b)", val);
            }
        }
        else if (!strcmp(key, "middle_range_color")) {
            if (parse_rgb(val, &r, &g, &b)) {
                g_middle_r = r; g_middle_g = g; g_middle_b = b;
            } else {
                LOGI("conf: bad [charge] middle_range_color=%s (want r,g,b)", val);
            }
        }
        else if (!strcmp(key, "upper_range_color")) {
            if (parse_rgb(val, &r, &g, &b)) {
                g_upper_r = r; g_upper_g = g; g_upper_b = b;
            } else {
                LOGI("conf: bad [charge] upper_range_color=%s (want r,g,b)", val);
            }
        }
        else if (!strcmp(key, "rise"))     g_c_rise   = atoi(val);
        else if (!strcmp(key, "hold"))     g_c_hold   = atoi(val);
        else if (!strcmp(key, "fall"))     g_c_fall   = atoi(val);
        else if (!strcmp(key, "offt"))     g_c_offt   = atoi(val);
        return;
    }
    if (!strcmp(sec, "notify")) {
        if      (!strcmp(key, "default_color")) {
            if (parse_rgb(val, &r, &g, &b)) {
                g_ntf_r = r; g_ntf_g = g; g_ntf_b = b;
            } else {
                LOGI("conf: bad [notify] default_color=%s (want r,g,b)", val);
            }
        }
        else if (!strcmp(key, "notify_light_type")) {
            if (parse_type(val, &g_ntf_type) == 0)
                LOGI("conf: bad [notify] notify_light_type=%s (0-3)", val);
        }
        else if (!strcmp(key, "rise"))         g_n_rise    = atoi(val);
        else if (!strcmp(key, "hold"))         g_n_hold    = atoi(val);
        else if (!strcmp(key, "fall"))         g_n_fall    = atoi(val);
        else if (!strcmp(key, "offt"))         g_n_offt    = atoi(val);
        else if (!strcmp(key, "notif_max_sec")) g_notif_max = atol(val);
        return;
    }
    if (!strcmp(sec, "rules")) {
        if (!parse_rgb(val, &r, &g, &b)) {
            LOGI("conf: bad [rules] %s=%s (want r,g,b)", key, val);
            return;
        }
        /* file wins over builtin for the same package */
        for (int i = 0; i < g_nrules; i++) {
            if (!strcmp(g_rules[i].pkg, key)) {
                g_rules[i].r = r; g_rules[i].g = g; g_rules[i].b = b;
                return;
            }
        }
        if (g_nrules < MAX_RULES) {
            snprintf(g_rules[g_nrules].pkg, sizeof(g_rules[0].pkg), "%s", key);
            g_rules[g_nrules].r = r;
            g_rules[g_nrules].g = g;
            g_rules[g_nrules].b = b;
            g_nrules++;
        }
    }
}

static void load_file(void)
{
    reset_dynamic();
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        LOGI("conf: no %s, using builtins", CONF_PATH);
        return;
    }
    char sec[32] = "";
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char *p = strchr(line, ']');
            if (!p) continue;
            *p = '\0';
            snprintf(sec, sizeof(sec), "%s", line + 1);
            continue;
        }
        if (!strcmp(sec, "suppress")) {
            if (!in_supp(line) && g_nsupp < MAX_SUPP)
                snprintf(g_supp[g_nsupp++], sizeof(g_supp[0]), "%s", line);
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq && (eq > line)) {
            *eq = '\0';
            trim(line);
            char *val = eq + 1;
            trim(val);
            parse_value(sec, line, val);
        }
    }
    fclose(f);
    LOGI("conf: loaded %s (%d suppressed, %d rules)", CONF_PATH, g_nsupp, g_nrules);
}

/* ---------------- public early hooks (called from core) ---------------- */

void conf_maybe_reload(void)
{
    struct stat st;
    if (stat(CONF_PATH, &st) != 0) {
        /* file gone: if we had anything dynamic beyond builtins, reset */
        if (g_last_mtime != 0) { reset_dynamic(); g_last_mtime = 0; }
        return;
    }
    if (st.st_mtime == g_last_mtime) return;
    g_last_mtime = st.st_mtime;
    load_file();
}

/* returns 1 if suppressed, 0 otherwise (config [suppress] only) */
int conf_suppressed(const char *pkg)
{
    conf_maybe_reload();
    return in_supp(pkg);
}

/* returns 1 and the r,g,b colour if the package has a rule (builtin or
 * config overridden), 0 otherwise (core falls back to the default colour) */
int conf_pkg_rgb(const char *pkg, int *r, int *g, int *b)
{
    conf_maybe_reload();
    return rule_rgb(pkg, r, g, b);
}


int conf_first_threshold(void)
{
	conf_maybe_reload(); return g_first_at;
}

int conf_second_threshold(void)
{
	conf_maybe_reload(); return g_second_at;
}


void conf_charge_lower_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload(); *r = g_lower_r; *g = g_lower_g; *b = g_lower_b;
}

void conf_charge_middle_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload(); *r = g_middle_r; *g = g_middle_g; *b = g_middle_b;
}

void conf_charge_upper_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload(); *r = g_upper_r; *g = g_upper_g; *b = g_upper_b;
}


int conf_charge_lower_type(void)
{
	conf_maybe_reload(); return g_lower_type;
}

int conf_charge_middle_type(void)
{
	conf_maybe_reload(); return g_middle_type;
}

int conf_charge_upper_type(void)
{
	conf_maybe_reload(); return g_upper_type;
}


void conf_notif_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload(); *r = g_ntf_r; *g = g_ntf_g; *b = g_ntf_b;
}

int conf_notif_type(void)
{
	conf_maybe_reload(); return g_ntf_type;
}


void conf_charge_timing(int *rise, int *hold, int *fall, int *offt)
{
    conf_maybe_reload();
    *rise = g_c_rise; *hold = g_c_hold; *fall = g_c_fall; *offt = g_c_offt;
}

void conf_notif_timing(int *rise, int *hold, int *fall, int *offt)
{
    conf_maybe_reload();
    *rise = g_n_rise; *hold = g_n_hold; *fall = g_n_fall; *offt = g_n_offt;
}


long conf_notif_max_sec(void)
{
	conf_maybe_reload(); return g_notif_max;
}



