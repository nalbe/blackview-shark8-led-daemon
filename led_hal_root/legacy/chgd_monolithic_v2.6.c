/*
 * chgd v2 - single event-driven LED daemon for led_hal_root module
 * Replaces: worker.sh + listener.sh + chgd v1
 *
 * Sources (all poll()-based, zero busy loops):
 *   1. NETLINK_KOBJECT_UEVENT group 1
 *        - power_supply/* events -> recompute charge band -> sysfs LEDs
 *        - fb0 events            -> refresh cached screen state
 *   2. /dev/socket/logdr  "stream tail=1 lids=2"   (events buffer)
 *        - parses notification_enqueue binary entries -> pkg name
 *        - arms hardware breathing on notification LEDs
 *        - dialer pings are verified against call_log (type=3 new=1,
 *          fresh) before arming the blue missed-call breathing
 *   3. adaptive timerfd (no fixed 30s poll)
 *        - armed notification : 1s screen re-check + NOTIF_MAX_SEC cap
 *        - logdr disconnected : RETRY_SEC reconnect backoff
 *        - idle, all links up : WATCHDOG_SEC safety refresh only
 *        All state transitions are triggered by uevents / events stream /
 *        signals; the timer is retuned on every transition.
 *
 * LED behavior (parity with worker.sh v2.6):
 *   charge:  cap<90 red breathe | 90..94 amber breathe | >=95/Full solid green
 *   notify:  hw breathing, mask bits {4:red,2:green,1:blue}, wins over charge
 *   screen on -> notifications dropped/disarmed (lcd-backlight>0)
 *
 * Compat: still writes /data/local/tmp/led_chg "<band> <epoch>".
 * Test hooks: kill -USR1 $(pidof chgd) -> fake telegram enqueue
 *             kill -USR2 $(pidof chgd) -> disarm notification
 * Modes: (no args) run | --once : write state file once | -v verbose stderr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <poll.h>
#include <linux/netlink.h>

#define STATE_PATH "/data/local/tmp/led_chg"
#define STATE_TMP  "/data/local/tmp/led_chg.tmp"
#define LOCK_PATH  "/data/local/tmp/led_chgd.lock"
#define LOGDR_SOCK "/dev/socket/logdr"
#define LOGDR_REQ  "stream tail=1 lids=2"
#define TAGS_FILE  "/system/etc/event-log-tags"
#define TAG_FALLBACK 27501            /* AOSP notification_enqueue */

#define AMBER_AT 90
#define GREEN_AT 95

#define RISE 500
#define HOLD 100
#define FALL 500
#define OFFT 1200

#define C_RISE 700
#define C_HOLD 100
#define C_FALL 700
#define C_OFFT 900

#define ARMED_SEC    1     /* screen watch while notification armed */
#define RETRY_SEC    3     /* logdr reconnect backoff */
#define WATCHDOG_SEC 300   /* idle safety refresh (missed uevents) */
#define NOTIF_MAX_SEC 1800

#define RING_MAX_SEC  120   /* hard cap on rainbow even if state sticks */

#define DIALER_PKG       "com.google.android.dialer"
#define CALL_FRESH_MS    120000LL  /* ignore missed rows older than this */
#define CALL_CHECKS      4         /* verification attempts per ping */
#define CALL_RECHECK_SEC 2
#define INCOMING_PKG     "incoming.call"   /* pseudo-pkg: ring rainbow mode */
#define RING_STEP_MS     32        /* fade tick: 128 ticks ~= 4.1s cycle */
#define RING_CHK_SEC     1         /* telephony state re-poll period */

static int g_verbose = 0;

static void log_line(const char *fmt, ...)
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

#define LOGI(...) log_line(__VA_ARGS__)

/* ---------------- sysfs helpers ---------------- */

static int read_line(const char *path, char *out, size_t n)
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

static void write_sys(const char *path, const char *val)
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

static void led_set(const char *c, int on_bright)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/class/leds/%s/trigger", c);
    write_sys(p, "none");
    snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", c);
    char v[8]; snprintf(v, sizeof(v), "%d", on_bright ? 255 : 0);
    write_sys(p, v);
}

static void led_solid(const char *c)
{
    char p[128];
    /* stop hw pulsing first: stale led_time keeps the LED breathing */
    snprintf(p, sizeof(p), "/sys/class/leds/%s/blink", c);
    write_sys(p, "0");
    snprintf(p, sizeof(p), "/sys/class/leds/%s/led_time", c);
    write_sys(p, "0 0 0 0");
    led_set(c, 255);
}

static void led_breathe(const char *c, int rise, int hold, int fall, int offt)
{
    char p[128], v[64];
    led_set(c, 255);
    snprintf(p, sizeof(p), "/sys/class/leds/%s/led_time", c);
    snprintf(v, sizeof(v), "%d %d %d %d", rise, hold, fall, offt);
    write_sys(p, v);
    snprintf(p, sizeof(p), "/sys/class/leds/%s/blink", c);
    write_sys(p, "1");
}

static void leds_all_off(void)
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
static void set_rgb(int r, int g, int b)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", r);
    write_sys("/sys/class/leds/red/brightness", v);
    snprintf(v, sizeof(v), "%d", g);
    write_sys("/sys/class/leds/green/brightness", v);
    snprintf(v, sizeof(v), "%d", b);
    write_sys("/sys/class/leds/blue/brightness", v);
}

/* ---------------- ring rainbow ---------------- */

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

/* ---------------- screen state ---------------- */

/* returns 1 = screen on, 0 = off */
static int screen_on(void)
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

/* ---------------- charge band ---------------- */

static const char *band_for(const char *status, const char *cap)
{
    int c = -1;
    if (cap && *cap) {
        c = atoi(cap);
        for (const char *p = cap; *p; p++)
            if (*p < '0' || *p > '9') { c = -1; break; }
    }

    if (!strcmp(status, "Full"))
        return "green";

    if (!strcmp(status, "Charging")) {
        if (c >= GREEN_AT) return "green";
        if (c >= AMBER_AT) return "amber";
        return "red";
    }

    if (!strcmp(status, "Not charging"))
        return (c >= GREEN_AT) ? "green" : "none";

    return "none";
}

static long eval_and_write(void)
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

/* ---------------- notification LED logic ---------------- */

struct notif_state {
    char   cur_pkg[96];     /* armed package, "" = idle */
    time_t armed_at;
};

static struct notif_state g_st;     /* single instance, shared with timer */

/* ---------------- adaptive timer ---------------- */

/* One timerfd serves all pending timeouts; it is re-armed on every state
 * transition instead of ticking blindly every 30s:
 *   notification armed -> 1s screen watch (NOTIF_MAX_SEC cap still applies)
 *   dialer ping seen   -> 2s missed-call verification window
 *   logdr down         -> RETRY_SEC reconnect backoff
 *   idle, links up     -> WATCHDOG_SEC safety refresh only */
static int g_tfd = -1;
static int g_ld  = -1;

/* missed-call verification state, shared with retune_timer() policy */
static long long g_last_missed_id   = -1;
static int       g_call_checks_left = 0;
static time_t    g_next_call_check  = 0;

static void retune_timer(void)
{
    if (g_tfd < 0) return;
    const char *why;
    long ms;
    int is_ring = g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, INCOMING_PKG);
    if (is_ring)                     { ms = RING_STEP_MS;          why = "ring rainbow"; }
    else if (g_st.cur_pkg[0])        { ms = ARMED_SEC * 1000L;     why = "armed"; }
    else if (g_call_checks_left > 0) { ms = CALL_RECHECK_SEC*1000L;why = "call check"; }
    else if (g_ld < 0)               { ms = RETRY_SEC * 1000L;     why = "logdr retry"; }
    else                             { ms = WATCHDOG_SEC * 1000L;  why = "watchdog"; }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_sec  = ms / 1000L;
    its.it_interval.tv_nsec = (ms % 1000L) * 1000000L;
    its.it_value = its.it_interval;
    /* skip if already running this policy (avoids resetting the countdown
     * and duplicate log lines when several paths retune back-to-back) */
    struct itimerspec cur;
    long curms;
    if (timerfd_gettime(g_tfd, &cur) == 0) {
        curms = (long)cur.it_interval.tv_sec * 1000L +
                cur.it_interval.tv_nsec / 1000000L;
        if (curms == ms && (cur.it_value.tv_sec || cur.it_value.tv_nsec))
            return;
    }
    timerfd_settime(g_tfd, 0, &its, NULL);
    LOGI("timer -> %ldms (%s)", ms, why);
}


static int mask_for(const char *pkg)
{
    if (!strcmp(pkg, "com.whatsapp"))                      return 2; /* green  */
    if (!strcmp(pkg, "org.telegram.messenger"))            return 5; /* r+b    */
    if (!strcmp(pkg, "com.google.android.apps.messaging")) return 2; /* green  */
    if (!strcmp(pkg, "com.viber.voip"))                    return 1; /* blue   */
    if (!strcmp(pkg, "missed.call"))                       return 1; /* blue   */
    return 7;                                                    /* white  */
}

static int suppressed(const char *pkg)
{
    static const char *list[] = {
        "", "com.android.systemui", "android",
        "com.android.shell", "org.amnezia.vpn",
    };
    for (size_t i = 0; i < sizeof(list)/sizeof(list[0]); i++)
        if (!strcmp(pkg, list[i])) return 1;
    return 0;
}

/* last band physically applied to the LED nodes; rewrite only on change,
 * otherwise every tick/uevent would blink the LED off->on */
static char g_applied_band[16] = "\x01INIT";

static void apply_charge_leds(void)
{
    char line[64] = "";
    read_line(STATE_PATH, line, sizeof(line));
    char *sp = strchr(line, ' ');
    if (sp) *sp = '\0';
    const char *band = line;

    if (!strcmp(g_applied_band, band))
        return;

    leds_all_off();
    if (!strcmp(band, "red")) {
        led_breathe("red", C_RISE, C_HOLD, C_FALL, C_OFFT);
    } else if (!strcmp(band, "amber")) {
        led_breathe("red",   C_RISE, C_HOLD, C_FALL, C_OFFT);
        led_breathe("green", C_RISE, C_HOLD, C_FALL, C_OFFT);
    } else if (!strcmp(band, "green")) {
        led_solid("green");
    }
    snprintf(g_applied_band, sizeof(g_applied_band), "%s", band);
    LOGI("charge leds -> %s", band);
}

static void arm_notification(struct notif_state *st, const char *pkg)
{
    int m = mask_for(pkg);
    g_applied_band[0] = '\0';       /* LEDs taken over: force reapply later */
    leds_all_off();
    if (m & 4) led_breathe("red",   RISE, HOLD, FALL, OFFT);
    if (m & 2) led_breathe("green", RISE, HOLD, FALL, OFFT);
    if (m & 1) led_breathe("blue",  RISE, HOLD, FALL, OFFT);
    snprintf(st->cur_pkg, sizeof(st->cur_pkg), "%s", pkg);
    st->armed_at = time(NULL);
    retune_timer();
    LOGI("notify armed: %s mask=%d", pkg, m);
}

static void disarm_notification(struct notif_state *st, const char *why)
{
    if (!st->cur_pkg[0]) return;
    LOGI("notify disarmed (%s): %s", why, st->cur_pkg);
    st->cur_pkg[0] = '\0';
    apply_charge_leds();
    retune_timer();
}

/* ---------------- incoming call: rainbow ---------------- */

/* Entered when a dialer ping coincides with mCallState==RINGING
 * (dumpsys telephony.registry). The timer drops to RING_STEP_MS and each
 * tick cross-fades to the next palette color; exit is handled by the tick:
 *   ringing gone -> resolve outcome (missed row -> blue breath,
 *                  answered/reset -> back to charge leds)
 * Screen state is DELIBERATELY ignored while ringing: this ROM lights up
 * the display for incoming calls and the LED must outlive it. Safety net
 * is RING_MAX_SEC in case a state event is lost. */
static time_t g_last_tele_chk;
/* 1 if the current rainbow was caused by an incoming (RINGING) call;
 * 0 for an outgoing (OFFHOOK) call. On end, incoming resolves to a missed
 * call check, outgoing just drops back to the charge leds. */
static int g_ring_incoming;

static void arm_ring(int incoming)
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

/* ---------------- missed calls ---------------- */

/* No dedicated kernel/eventlog event exists for a missed call. The dialer
 * (com.google.android.dialer) posts a notification for every call-related
 * state, so pkg alone cannot distinguish "incoming ringing" / "ongoing"
 * from an actual missed call. Verification: when the dialer pings us we
 * query content://call_log/calls (root) for a fresh MISSED row
 * (type=3, new=1). A couple of rechecks absorb the provider/db race.
 * Dedup by call_log _id: dialer notification updates must not re-arm. */

#define DIALER_PKG       "com.google.android.dialer"

static int is_dialer(const char *pkg)
{
    return !strcmp(pkg, DIALER_PKG);
}

/* Fork/exec a command, capture stdout (capped), 5s hard timeout.
 * SIGCHLD is SIG_IGN so children are auto-reaped. Returns bytes captured. */
static size_t run_capture(char *const argv[], char *out, size_t cap)
{
    int fds[2];
    if (pipe(fds)) return 0;
    pid_t p = fork();
    if (p < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (p == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);

    size_t got = 0;
    struct pollfd pf = { .fd = fds[0], .events = POLLIN, .revents = 0 };
    for (;;) {
        int r = poll(&pf, 1, 5000);          /* hard cap: never stall loop */
        if (r <= 0) break;
        if (got >= cap - 1) break;
        ssize_t n = read(fds[0], out + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fds[0]);
    kill(p, SIGKILL);                        /* SIGCHLD=SIG_IGN: auto-reaped */
    out[got] = '\0';
    return got;
}

/* 1 = RINGING (incoming), 0 = not an incoming ring (idle/offhook) */
static int tele_ringing(void)
{
    static char out[16384];
    char *argv[] = {
        (char *)"/system/bin/dumpsys", (char *)"telephony.registry", NULL
    };
    if (!run_capture(argv, out, sizeof(out))) return 0;
    int ringing = 0;                         /* multi-SIM: any line counts */
    for (char *s = strstr(out, "mCallState="); s;
         s = strstr(s + 11, "mCallState="))
        if (atoi(s + 11) == 1) { ringing = 1; break; }
    return ringing;
}

/* Any active call (incoming RINGING=1 or outgoing OFFHOOK=2). The dialer
 * posts a notification_enqueue for outgoing calls too (channel
 * phone_ongoing_call), so a single event-time telephony probe distinguishes
 * "an active call is in progress" without adding any polling loop. */
static int tele_active(void)
{
    static char out[16384];
    char *argv[] = {
        (char *)"/system/bin/dumpsys", (char *)"telephony.registry", NULL
    };
    if (!run_capture(argv, out, sizeof(out))) return 0;
    int active = 0;                          /* multi-SIM: any line counts */
    for (char *s = strstr(out, "mCallState="); s;
         s = strstr(s + 11, "mCallState=")) {
        int cs = atoi(s + 11);
        if (cs == 1 || cs == 2) { active = 1; break; }
    }
    return active;
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
    g_next_call_check = time(NULL) + CALL_RECHECK_SEC;

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

/* Called from the tick and right after a dialer ping. */
static void maybe_call_check(void)
{
    if (g_call_checks_left <= 0 || time(NULL) < g_next_call_check) return;
    check_missed_call();
}

static void handle_enqueue(struct notif_state *st, const char *pkg)
{
    LOGI("enqueue event: %s", pkg);
    if (suppressed(pkg)) {
        LOGI("suppressed: %s", pkg);
        return;
    }
    if (is_dialer(pkg)) {
        /* could be ringing / ongoing / missed -> disambiguate */
        if (!strcmp(st->cur_pkg, INCOMING_PKG))
            return;                          /* rainbow already running */
        LOGI("dialer ping");
        if (tele_active()) {
            /* Incoming (RINGING=1) or outgoing (OFFHOOK=2) call is live
             * right now -> rainbow. Everything below is event-triggered
             * by this dialer ping, no polling loop involved. */
            arm_ring(tele_ringing());   /* 1 incoming, 0 outgoing */
            return;
        }
        /* not in a call right now: maybe the missed row lands slightly
         * later -> open a short verification window */
        g_call_checks_left = CALL_CHECKS;
        g_next_call_check = 0;               /* first check right away */
        maybe_call_check();
        return;
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

/* ---------------- events-buffer parsing ---------------- */

static unsigned g_tag_enq = TAG_FALLBACK;

static void load_tag_id(void)
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

static int parse_entry(const unsigned char *buf, size_t n,
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

/* ---------------- logdr connection ---------------- */

static int logdr_connect(void)
{
    g_ld = -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, LOGDR_SOCK, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    if (send(fd, LOGDR_REQ, strlen(LOGDR_REQ), 0) < 0) {
        close(fd);
        return -1;
    }
    g_ld = fd;
    LOGI("logdr connected (%s)", LOGDR_REQ);
    return fd;
}

/* ---------------- signals ---------------- */

static volatile sig_atomic_t g_fake_enq   = 0;
static volatile sig_atomic_t g_fake_dial  = 0;
static volatile sig_atomic_t g_fake_ring  = 0;
static volatile sig_atomic_t g_clear      = 0;

static void on_usr1(int s){ (void)s; g_fake_enq = 1; }
static void on_usr2(int s){ (void)s; g_clear = 1; }
static void on_hup (int s){ (void)s; g_fake_dial = 1; }
static void on_winch(int s){ (void)s; g_fake_ring = 1; }

static void install_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;              /* no SA_RESTART: select -> EINTR */
    sigaction(sig, &sa, NULL);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }

    if (once) {
        load_tag_id();
        eval_and_write();
        printf("%s", "");
        char buf[64] = "";
        read_line(STATE_PATH, buf, sizeof(buf));
        fputs(buf, stdout);
        return 0;
    }

    int lfd = open(LOCK_PATH, O_RDWR | O_CREAT, 0666);
    if (lfd < 0) { perror("open lock"); return 1; }
    if (flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "chgd: another instance owns the lock\n");
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    install_handler(SIGUSR1, on_usr1);
    install_handler(SIGUSR2, on_usr2);
    install_handler(SIGHUP,  on_hup);
    install_handler(SIGWINCH, on_winch);

    load_tag_id();

    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (nl < 0) { perror("socket"); return 1; }
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (bind(nl, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind netlink");
        return 1;
    }
    /* never let a spurious readiness block us */
    fcntl(nl, F_SETFL, fcntl(nl, F_GETFL, 0) | O_NONBLOCK);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd"); return 1; }
    g_tfd = tfd;
    /* disarmed until first retune_timer() below */

    (void)eval_and_write();
    apply_charge_leds();

    memset(&g_st, 0, sizeof(g_st));

    logdr_connect();                /* sets g_ld; may fail, retried on timer */

    static unsigned char buf[16384];
    LOGI("chgd v2 running (nl=%d tfd=%d ld=%d)", nl, tfd, g_ld);
    retune_timer();

    for (;;) {
        /* --- test hooks (also run on EINTR restarts) --- */
        if (g_fake_enq) {
            g_fake_enq = 0;
            handle_enqueue(&g_st, "org.telegram.messenger");
        }
        if (g_fake_dial) {
            g_fake_dial = 0;
            handle_enqueue(&g_st, DIALER_PKG);
        }
        if (g_fake_ring) {
            g_fake_ring = 0;
            if (!g_st.cur_pkg[0]) arm_ring(0);  /* rainbow test w/o call */
        }
        if (g_clear) {
            g_clear = 0;
            disarm_notification(&g_st, "sigusr2");
        }

        int maxfd = nl > tfd ? nl : tfd;
        if (g_ld > maxfd) maxfd = g_ld;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(nl, &rfds);
        FD_SET(tfd, &rfds);
        if (g_ld >= 0) FD_SET(g_ld, &rfds);

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal: flags handled above */
            perror("select");
            break;
        }
        if (rc == 0) continue;

        /* --- kernel uevents: drain everything queued --- */
        if (FD_ISSET(nl, &rfds)) {
            for (;;) {
                ssize_t n = recv(nl, buf, sizeof(buf) - 1, MSG_DONTWAIT);
                if (n <= 0) break;
                buf[n] = '\0';
                if (strstr((char *)buf, "/power_supply/")) {
                    (void)eval_and_write();
                    if (!g_st.cur_pkg[0])
                        apply_charge_leds();
                } else if (strstr((char *)buf, "fb0") ||
                           strstr((char *)buf, "lcd")) {
                    /* ring rainbow ignores the screen: the incoming-call
                     * UI wakes the display and must not kill the effect */
                    if (g_st.cur_pkg[0] &&
                        strcmp(g_st.cur_pkg, INCOMING_PKG) &&
                        screen_on())
                        disarm_notification(&g_st, "screen on");
                }
            }
        }

        /* --- events stream --- */
        if (g_ld >= 0 && FD_ISSET(g_ld, &rfds)) {
            ssize_t n = recv(g_ld, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) {
                LOGI("logdr closed (n=%zd errno=%d), retry in %ds",
                     n, errno, RETRY_SEC);
                close(g_ld);
                g_ld = -1;
                retune_timer();         /* switch to reconnect backoff now */
            } else {
                char pkg[96];
                int hit = parse_entry(buf, (size_t)n, pkg, sizeof(pkg));
                if (g_verbose && (size_t)n >= 32) {
                    unsigned hs = buf[2] | (buf[3] << 8);
                    LOGI("ldpkt n=%zd hdr=%u tag=%u parse=%d pkg=%s",
                         n, hs, le32(buf + hs), hit,
                         hit ? pkg : "-");
                }
                if (hit)
                    handle_enqueue(&g_st, pkg);
            }
        }

        /* --- adaptive housekeeping --- */
        if (FD_ISSET(tfd, &rfds)) {
            uint64_t exp;
            ssize_t ig = read(tfd, &exp, sizeof(exp)); (void)ig;

            if (g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, INCOMING_PKG)) {
                /* --- ring rainbow mode (screen state ignored) --- */
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
                            g_call_checks_left = CALL_CHECKS;
                            g_next_call_check = time(NULL) + 1;
                        }
                    }
                }
                double age = difftime(time(NULL), g_st.armed_at);
                if (age >= RING_MAX_SEC)
                    disarm_notification(&g_st, "timeout");
            } else {
                maybe_call_check();      /* pending verification window */

                if (g_st.cur_pkg[0]) {
                    LOGI("armed tick");
                    double age = difftime(time(NULL), g_st.armed_at);
                    if (age >= NOTIF_MAX_SEC)
                        disarm_notification(&g_st, "timeout");
                    else if (screen_on())
                        disarm_notification(&g_st, "screen on (tick)");
                } else {
                    if (g_ld < 0) {
                        logdr_connect();
                        if (g_ld >= 0)
                            apply_charge_leds();   /* band may have changed */
                    } else if (g_call_checks_left <= 0) {
                        LOGI("watchdog tick");
                    }
                    /* safety refresh in case a uevent was missed */
                    (void)eval_and_write();
                    if (!g_st.cur_pkg[0])
                        apply_charge_leds();
                }
            }

            /* policy may have changed inside the branches above */
            retune_timer();
        }
    }

    leds_all_off();
    return 1;
}
