/*
 * mods/voip.c - messenger call (VoIP) rainbow mode.
 *
 * Lights the rainbow when a messenger (Telegram / WhatsApp / Viber / Signal)
 * is actually in a call, not for ordinary chat messages. A chat message has
 * no voice channel, a call does - so we detect it by the audio policy:
 *   dumpsys media.audio_policy -> "Phone state: AUDIO_MODE_IN_COMMUNICATION"
 * (a live / accepted VoIP call) or "AUDIO_MODE_IN_RINGTONE" (an unanswered
 * incoming VoIP ring). Both appear only while a voice call is in progress,
 * are shared by every messenger (they all request voice-communication audio
 * focus through the audio service) and are not tied to the SIM radio.
 *
 * Trigger path: mods/notify.c's default handler asks voip_try() for every
 * unclaimed package BEFORE it colors it normally. voip_try() resolves:
 *   package not a messenger                       -> 0 (normal color path)
 *   messenger but no voice channel (chat message) -> 0 (normal color path)
 *   messenger + active/incoming voice channel     -> arm the rainbow, 1
 *   already ringing                                -> 1 (ignore, keep rainbow)
 * So a chat keeps its [rules] color, a call gets the rainbow.
 *
 * Ownership is a MODE exactly like mods/ring.c: while the pseudo-package
 * "voip.call" is armed the core drops the timer to the 32ms rainbow tick
 * (the same cross-fade cycler in led.c) and voip_tick() steps it plus
 * re-polls the audio state at a slower cadence to detect call end.
 *
 * Config: [voip] max_sec=<n> = grace timeout in seconds while the voice
 *         channel reports "no call" before the rainbow is disarmed (default
 *         VOIP_MAX_SEC). It guards against a lost call-end event and against
 *         a momentary NORMAL blip in the audio policy; it is NOT a hard cap,
 *         so a long live call keeps the rainbow for its whole duration.
 *         [voip] packages=c1,c2,... overrides the messenger list.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define VOIP_PKG      "voip.call"
#define VOIP_STEP_MS  32          /* rainbow fade tick (led.c cycler) */
#define VOIP_AUDIO_MS 1000        /* audio-state re-poll cadence */
#define VOIP_MAX_SEC  2           /* grace: no-call seconds before disarm */

/* default messenger packages (comma list string, also the [voip] fallback) */
#define VOIP_DEF_PKGS "org.telegram.messenger," \
                       "com.whatsapp," \
                       "com.viber.voip," \
                       "org.thoughtcrime.securesms," \
                       "com.snapchat," \
                       "com.google.android.apps.tachyon"

static time_t g_last_audio_chk;
static time_t g_last_alive;      /* last tick where the call was confirmed */

int voip_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, VOIP_PKG);
}

/* the configured messenger package list (for the per-marker checks) */
static const char *voip_pkg_list(void)
{
    const char *list = conf_get_str("voip", "packages");
    return (list && list[0]) ? list : VOIP_DEF_PKGS;
}

/* source 1: audio policy voice channel. True for a live/accepted VoIP call
 * (AUDIO_MODE_IN_COMMUNICATION) and - on ROMs that expose it - for an
 * unanswered incoming ring (AUDIO_MODE_IN_RINGTONE). */
static int voip_audio_comm(void)
{
    static char out[8192];
    char *argv[] = {
        (char *)"/system/bin/dumpsys", (char *)"media.audio_policy", NULL
    };
    if (!run_capture(argv, out, sizeof(out))) return 0;
    char *s = strstr(out, "Phone state:");
    if (!s) return 0;
    return strstr(s, "IN_COMMUNICATION") || strstr(s, "IN_RINGTONE");
}

/* source 2: telecom. An incoming unanswered messenger ring is its own
 * self-managed (VoIP) ConnectionService call in `dumpsys telecom`, listed
 * as RINGING / ACTIVE, so it is visible even when the audio policy still
 * says NORMAL (which is exactly the incoming-ring case audio misses). */
static int voip_telecom_call(void)
{
    static char out[32768];
    char *argv[] = {
        (char *)"/system/bin/dumpsys", (char *)"telecom", NULL
    };
    if (!run_capture(argv, out, sizeof(out))) return 0;
    if (!(strstr(out, "RINGING") || strstr(out, "ACTIVE, ") ||
          strstr(out, "DIALING, ") || strstr(out, "state: ACTIVE")))
        return 0;
    const char *list = voip_pkg_list();
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        if (L > 0 && strstr(out, p)) return 1;   /* p not NUL-terminated, substring ok */
        if (!e) break;
        p = e + 1;
        while (*p == ' ' || *p == ',') p++;
    }
    return 0;
}

/* Is a messenger voice call (incoming ring OR live call) up right now? */
static int voip_comm(void)
{
    return voip_audio_comm() || voip_telecom_call();
}

/* true if pkg is one of the configured messenger packages */
static int is_messenger(const char *pkg)
{
    if (!pkg || !pkg[0]) return 0;
    const char *list = voip_pkg_list();
    size_t pl = strlen(pkg);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        if (L == pl && !strncmp(p, pkg, L)) return 1;
        if (!e) break;
        p = e + 1;
        while (*p == ' ' || *p == ',') p++;
    }
    return 0;
}

/* hard cap, overridable via [voip] max_sec */
static long voip_max_sec(void)
{
    long v = conf_get_int("voip", "max_sec", VOIP_MAX_SEC);
    return v > 0 ? v : VOIP_MAX_SEC;
}

/* ---------------- arming ---------------- */

static void arm_voip(void)
{
    g_applied_band[0] = '\0';       /* charge leds must reapply after */
    leds_all_off();                 /* kill breathing, all channels 0 */
    led_rainbow_reset();
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", VOIP_PKG);
    g_st.armed_at = time(NULL);
    g_last_audio_chk = g_st.armed_at;
    g_last_alive = g_st.armed_at;
    led_rainbow_step();             /* first color immediately */
    int r, gr, b;
    led_rainbow_rgb(&r, &gr, &b);
    status_write("voip", "", "voip.call", r, gr, b, 0);
    retune_timer();
    LOGI("voip ring -> rainbow");
}

/* ---------------- mode ---------------- */

static int voip_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, VOIP_PKG);
}

static void voip_tick(void)
{
    led_rainbow_step();
    double dt = difftime(time(NULL), g_last_audio_chk);
    if (dt >= VOIP_AUDIO_MS / 1000.0) {
        g_last_audio_chk = time(NULL);
        if (voip_comm())
            g_last_alive = g_last_audio_chk;   /* the call is really up */
    }
    /* disarm only when the voice channel has been silent for max_sec
     * in a row. A live call keeps g_last_alive fresh, so it can run far
     * beyond any fixed cap; a momentary NORMAL blip is likewise absorbed.
     * VOIP_MAX_SEC only bounds how long a LOST call-end event may linger. */
    double silent = difftime(time(NULL), g_last_alive);
    if (silent >= (double)voip_max_sec()) {
        LOGI("voip call ended -> charge leds");
        disarm_notification(&g_st, "voip end");
    }
}

/* Called by notify's default handler before it colors a package.
 * Returns 1 if voip took over the led (either it just armed the rainbow or
 * the rainbow is already running), 0 to leave the package to the normal
 * notification color path. */
int voip_try(const char *pkg)
{
    if (!is_messenger(pkg)) return 0;     /* not ours -> normal color */
    if (voip_active()) return 1;          /* already ringing: keep it */
    if (voip_comm()) {                    /* actual messenger call */
        arm_voip();
        return 1;
    }
    return 0;                              /* chat message -> normal color */
}

REGISTER_MODE("voip", VOIP_STEP_MS, voip_owns, voip_tick);
