/*
 * mods/dialer.c - missed-call / call rainbow verification.
 *
 * Claims every com.google.android.dialer notification via REGISTER_HANDLER.
 * No dedicated kernel/eventlog event exists for a missed call, so pkg alone
 * cannot distinguish "incoming ringing" / "ongoing" from an actual missed
 * call. Verification: on a dialer ping we probe telephony state (active
 * call -> ring rainbow via mods/ring.c) and otherwise query
 * content://call_log/calls (root) for a fresh MISSED row (type=3, new=1).
 * A couple of rechecks absorb the provider/db race. Dedup by call_log _id.
 *
 * REGISTER_HANDLER: any mod can claim a package's notifications and fully
 * own the response (see the dispatch in notify.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define DIALER_PKG     "com.google.android.dialer"
#define CALL_FRESH_MS  120000LL  /* ignore missed rows older than this */
#define CALL_CHECKS    4         /* verification attempts per ping */

/* missed-call verification state */
long long g_last_missed_id   = -1;
int       g_call_checks_left = 0;
time_t    g_next_call_check  = 0;

const char *dialer_pkg_id(void)
{
    return DIALER_PKG;
}

/* Run `content query` as root; returns 1 and the newest missed-call row
 * (rows come date DESC) if present in the output. */
static int newest_missed(long long *id, long long *date_ms)
{
    static char out[16384];
    char *argv[] = {
        (char *)"/system/bin/content", (char *)"query",
        (char *)"--uri",   (char *)"content://call_log/calls",
        (char *)"--projection", (char *)"_id:date:type:new:number",
        (char *)"--sort",  (char *)"date DESC",
        NULL
    };
    size_t got = run_capture(argv, out, sizeof(out));
    if (!got) return 0;

    /* Row: N _id=X, date=Y, type=T, new=N, number=... */
    int found = 0;
    long long best_id = -1, best_ts = -1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        long long rid = 0, rts = 0; int type = 0, isnew = 0;
        if (sscanf(line, "Row: %*d _id=%lld, date=%lld, type=%d, new=%d",
                   &rid, &rts, &type, &isnew) != 4)
            continue;
        if (type == 3 && isnew == 1 && rid > best_id) {
            best_id = rid; best_ts = rts; found = 1;
            break;                           /* first hit = newest missed */
        }
    }
    if (found) { *id = best_id; *date_ms = best_ts; }
    return found;
}

void dialer_reopen_window(void)
{
    g_call_checks_left = CALL_CHECKS;
    g_next_call_check = time(NULL) + 1;
}

static void check_missed_call(void)
{
    if (tele_active()) {            /* state flipped after the ping */
        g_call_checks_left = 0;
        arm_ring(tele_ringing());   /* 1 incoming, 0 outgoing */
        return;
    }

    long long id = 0, ts = 0;
    int found = newest_missed(&id, &ts);
    g_call_checks_left--;
    g_next_call_check = time(NULL) + (time_t)(CALL_RECHECK_MS / 1000L);

    if (found) {
        long long age = (long long)time(NULL) * 1000 - ts;
        if (id > g_last_missed_id && age >= 0 && age < CALL_FRESH_MS) {
            g_last_missed_id = id;
            g_call_checks_left = 0;
            LOGI("missed call id=%lld age=%lldms -> led", id, age);
            arm_notification(&g_st, "missed.call");
            retune_timer();
            return;
        }
        if (id > g_last_missed_id) {
            /* stale-but-unseen entry: consume it so it can't fire later */
            g_last_missed_id = id;
        }
    }

    retune_timer();                          /* recheck window or back to idle */
}

/* Called from the core tick every heartbeat. */
void maybe_call_check(void)
{
    if (g_call_checks_left <= 0 || time(NULL) < g_next_call_check) return;
    check_missed_call();
}

/* Claims every dialer notification. */
static int dialer_handle(const char *pkg)
{
    (void)pkg;
    if (ring_is_active())
        return 1;                          /* rainbow already running */
    LOGI("dialer ping");
    if (tele_active()) {
        /* Incoming (RINGING=1) or outgoing (OFFHOOK=2) call is live
         * right now -> rainbow. Everything below is event-triggered
         * by this dialer ping, no polling loop involved. */
        g_call_checks_left = 0;
        arm_ring(tele_ringing());   /* 1 incoming, 0 outgoing */
        return 1;
    }
    /* not in a call right now: maybe the missed row lands slightly
     * later -> open a short verification window */
    g_call_checks_left = CALL_CHECKS;
    g_next_call_check = 0;               /* first check right away */
    maybe_call_check();
    return 1;
}

REGISTER_HANDLER(DIALER_PKG, dialer_handle);

/* missed calls light blue like the rest of the call family */
REGISTER_RULE("missed.call", 0, 0, 255);