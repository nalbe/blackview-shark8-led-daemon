/*
 * core.c - front door and event loop of the modular chgd daemon.
 *
 * Owns the transport and the registries, no policy:
 *   - main loop: select() over netlink uevents, logdr socket, timerfd
 *   - netlink KOBJECT_UEVENT -> uev_dispatch(): every REGISTER_UEVENT
 *     hook whose match substring hits the raw message fires (charge
 *     owns "power_supply", notify owns "fb0"/"lcd" screen-on disarm)
 *   - logdr stream "tail=1 lids=2": raw events -> ev_parse() (the
 *     events-buffer decode lives here, it is transport) -> pkg_dispatch
 *     (exact-match claiming handlers first, then the "*" default)
 *   - adaptive timerfd: retune_timer() picks the heartbeat of the mode
 *     that owns g_st.cur_pkg - charge owns the idle channel (""),
 *     ring owns "incoming.call", notify owns any armed real package.
 *     Dialer's missed-call verification window and the logdr reconnect
 *     backoff win over idle while they are pending
 *   - signal test hooks and watchdog / lockfile housekeeping
 *
 * All policy lives in mods/: charge.c, notify.c, ring.c, dialer.c.
 * The core only walks the registry sections and calls shared services
 * declared in chgd.h (ev_parse, pkg_dispatch, uev_dispatch,
 * refresh_dispatch, retune_timer). No LED, no band, no arming here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "chgd.h"

#define STATE_PATH "/data/local/tmp/led_chg"
#define LOCK_PATH  "/data/local/tmp/led_chgd.lock"
#define LOGDR_SOCK "/dev/socket/logdr"
#define LOGDR_REQ  "stream tail=1 lids=2"

int g_tfd = -1;
int g_ld  = -1;

/* ---------------- shared armed state ---------------- */

struct notif_state g_st;               /* single instance, shared universe */

/* ---------------- events-buffer decode (core-owned transport) ---- */

static unsigned g_tag_enq = 27501;     /* AOSP notification_enqueue */

void ev_load_tags(void)
{
    FILE *f = fopen("/system/etc/event-log-tags", "r");
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

static unsigned le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

/*
 * Raw events entry: [logger_entry hdr][payload]
 *   payload: int32 tag_id (BE), then binary value list:
 *     byte type; INT(1)=int32 BE, STRING(3)=int32 len BE + bytes,
 *     LIST(4)=int32 content_len BE + nested values
 * enqueue fields: (uid int)(pid int)(pkg string)...
 * Wire format on this device (verified by raw dumps, little-endian).
 * Strict positional walking proved unreliable across tags (enqueue
 * carries an extra zero byte vs other tags), so after matching the tag
 * id we locate the package name by anchoring on its self-describing
 * string record:
 *   [0x02][u32le len][len bytes of [a-zA-Z0-9._] with >=1 dot][NUL]
 * which uniquely identifies the pkg field of notification_enqueue.
 */
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

/* ---------------- registry dispatch ---------------- */

/* any mode may own any pkg string, including "" (charge owns the idle
 * channel); modes themselves decide by pkg content */
const struct led_mode *mode_owns(const char *pkg)
{
    if (!pkg) return NULL;
    const struct led_mode *m;
    for (m = __start_chgd_modes; m < __stop_chgd_modes; m++)
        if (m->owns(pkg)) return m;
    return NULL;
}

/* exact-match claiming handlers first (dialer), then the "*" default
 * handler (notify's suppress/screen/dedup/arm pipeline). */
int pkg_dispatch(struct notif_state *st, const char *pkg)
{
    (void)st;
    const struct pkg_handler *h;
    for (h = __start_chgd_handlers; h < __stop_chgd_handlers; h++)
        if (strcmp(h->pkg, "*") && !strcmp(h->pkg, pkg) && h->fn(pkg)) {
            LOGI("claimed by handler: %s", pkg);
            return 1;
        }
    for (h = __start_chgd_handlers; h < __stop_chgd_handlers; h++)
        if (!strcmp(h->pkg, "*") && h->fn(pkg))
            return 1;
    return 0;
}

/* netlink KOBJECT_UEVENT consumers: charge ("power_supply"), notify
 * ("fb0"/"lcd" screen-on disarm). */
void uev_dispatch(const char *msg)
{
    const struct uevent_hook *h;
    for (h = __start_chgd_uevents; h < __stop_chgd_uevents; h++)
        if (strstr(msg, h->match))
            h->fn();
}

/* visible-state refreshers: run at boot and again on SIGALRM (led.conf
 * edited). charge.c re-evaluates its band and re-applies the LEDs. */
void refresh_dispatch(void)
{
    const struct refresh_hook *r;
    for (r = __start_chgd_refresh; r < __stop_chgd_refresh; r++)
        r->fn();
}

/* ---------------- adaptive timer ---------------- */

/* One timerfd serves all pending timeouts; it is re-armed on every state
 * transition instead of ticking blindly every 30s:
 *   dialer verification window open  -> 2s missed-call recheck
 *   logdr down, nothing armed       -> RETRY_SEC reconnect backoff
 *   mode owns cur_pkg               -> its heartbeat (charge 300s idle
 *                                      / notify 1s armed / ring 32ms)
 *   nothing owns (defensive)        -> WATCHDOG_SEC safety refresh
 * If the owning mode exposes an adaptive next_wake_ms(), that value wins:
 * it is programmed as a ONE-SHOT "sleep until the next real deadline"
 * (re-armed after every tick) instead of a fixed periodic heartbeat, so
 * notify may sleep for a minute when there is nothing to poll and only
 * wake for the timeout cap. */
void retune_timer(void)
{
    if (g_tfd < 0) return;
    const char *why;
    long ms;
    int one_shot = 0;
    if (g_call_checks_left > 0)              { ms = CALL_RECHECK_MS;   why = "call check"; }
    else if (g_ld < 0 && !g_st.cur_pkg[0])   { ms = RETRY_SEC * 1000L; why = "logdr retry"; }
    else {
        const struct led_mode *m = mode_owns(g_st.cur_pkg);
        if (m) {
            long w = m->next_wake_ms ? m->next_wake_ms() : 0;
            if (w > 0) {
                ms = w;
                one_shot = 1;            /* adaptive: re-armed after each tick */
                why = m->name;
            } else {
                ms = m->tick_ms;         /* plain periodic heartbeat */
                why = m->name;
            }
        } else                             { ms = WATCHDOG_SEC*1000L; why = "watchdog"; }
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = ms / 1000L;
    its.it_value.tv_nsec = (ms % 1000L) * 1000000L;
    /* periodic heartbeat only when the mode owns a fixed cadence; an
     * adaptive wake is a one-shot that the next tick re-arms */
    if (!one_shot) {
        its.it_interval.tv_sec  = its.it_value.tv_sec;
        its.it_interval.tv_nsec = its.it_value.tv_nsec;
    }
    /* skip if already running this policy (avoids resetting the countdown
     * and duplicate log lines when several paths retune back-to-back) */
    struct itimerspec cur;
    long curms;
    if (timerfd_gettime(g_tfd, &cur) == 0) {
        curms = (long)cur.it_value.tv_sec * 1000L +
                cur.it_value.tv_nsec / 1000000L;
        if (curms == ms && (cur.it_value.tv_sec || cur.it_value.tv_nsec))
            return;
    }
    timerfd_settime(g_tfd, 0, &its, NULL);
    LOGI("timer -> %ldms (%s)%s", ms, why, one_shot ? " one-shot" : "");
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
static volatile sig_atomic_t g_fake_charge = 0;
static volatile sig_atomic_t g_clear      = 0;
static volatile sig_atomic_t g_clear_log  = 0;
static volatile sig_atomic_t g_conf_reload = 0;

static void on_usr1(int s){ (void)s; g_fake_enq = 1; }
static void on_usr2(int s){ (void)s; g_clear = 1; }
static void on_hup (int s){ (void)s; g_fake_dial = 1; }
static void on_winch(int s){ (void)s; g_fake_ring = 1; }
static void on_quit (int s){ (void)s; g_fake_charge = 1; }
static void on_cont (int s){ (void)s; g_clear_log = 1; }
static void on_alrm(int s){ (void)s; g_conf_reload = 1; }

static void install_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;              /* no SA_RESTART: select -> EINTR */
    sigaction(sig, &sa, NULL);
}

/* ---------------- test dispatch ---------------- */

/* any test button takes over the LED unconditionally: disarm whatever
 * is active first (notification or ring), then arm the new test. */
static void test_disarm(const char *why)
{
    if (g_st.cur_pkg[0])
        disarm_notification(&g_st, why);
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
        ev_load_tags();
        eval_and_write();           /* mods/charge.c: band -> state file */
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
    install_handler(SIGQUIT, on_quit);
    install_handler(SIGCONT, on_cont);
    install_handler(SIGALRM, on_alrm);

    ev_load_tags();

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

    memset(&g_st, 0, sizeof(g_st));

    /* initial visible state: mods refresh themselves (charge band) */
    refresh_dispatch();

    logdr_connect();                /* sets g_ld; may fail, retried on timer */

    static unsigned char buf[16384];
    LOGI("chgd v2 running (nl=%d tfd=%d ld=%d)", nl, tfd, g_ld);
    retune_timer();

    for (;;) {
        /* --- test hooks (also run on EINTR restarts) --- */
        if (g_fake_enq) {
            g_fake_enq = 0;
            /* test telegram: arm directly, bypassing the screen-on guard
             * so the button works while the app is open in front of you */
            test_disarm("test switch");
            arm_notification_ex(&g_st, "org.telegram.messenger", 1);
        }
        if (g_fake_dial) {
            g_fake_dial = 0;
            /* test incoming call: rainbow, held (test mode ignores
             * telephony state, does not die from "call ended") */
            test_disarm("test switch");
            arm_ring_ex(1, 1);
        }
        if (g_fake_ring) {
            g_fake_ring = 0;
            test_disarm("test switch");
            arm_ring_ex(0, 1);  /* test rainbow, held */
        }
        if (g_fake_charge) {
            g_fake_charge = 0;
            /* test charge: cycle every band once (lower/middle/upper/
             * none), then hand the channel back to the idle refresh */
            test_disarm("test switch");
            arm_charge_test();
        }
        if (g_clear_log) {
            g_clear_log = 0;
            FILE *lf = fopen("/data/local/tmp/ledd.log", "w");
            if (lf) fclose(lf);     /* truncate */
            LOGI("log cleared (sigcont)");
        }
        if (g_clear) {
            g_clear = 0;
            disarm_notification(&g_st, "sigusr2");
        }
        if (g_conf_reload) {
            /* GUI just saved led.conf: force the lazy reload NOW and
             * let the visible-state mods re-apply whatever is active */
            g_conf_reload = 0;
            conf_maybe_reload();
            refresh_dispatch();
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
                uev_dispatch((char *)buf);
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
                int hit = ev_parse(buf, (size_t)n, pkg, sizeof(pkg));
                if (g_verbose && (size_t)n >= 32) {
                    unsigned hs = buf[2] | (buf[3] << 8);
                    LOGI("ldpkt n=%zd hdr=%u tag=%u parse=%d pkg=%s",
                         n, hs, (unsigned)buf[hs] | ((unsigned)buf[hs+1] << 8) |
                         ((unsigned)buf[hs+2] << 16) |
                         ((unsigned)buf[hs+3] << 24), hit,
                         hit ? pkg : "-");
                }
                if (hit)
                    pkg_dispatch(&g_st, pkg);
            }
        }

        /* --- adaptive housekeeping --- */
        if (FD_ISSET(tfd, &rfds)) {
            uint64_t exp;
            ssize_t ig = read(tfd, &exp, sizeof(exp)); (void)ig;

            /* transport retry (idle only, same as the mode cadence) */
            if (!g_st.cur_pkg[0] && g_ld < 0)
                logdr_connect();

            /* dialer verification window: no-op when closed */
            maybe_call_check();

            /* the mode that owns cur_pkg does the policy tick */
            const struct led_mode *m = mode_owns(g_st.cur_pkg);
            if (m) m->tick();
            else   LOGI("no mode owns %s", g_st.cur_pkg);

            /* policy may have changed inside the branches above */
            retune_timer();
        }
    }

    leds_all_off();
    return 1;
}