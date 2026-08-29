/*
 * chgd.c - core of the modular led_hal_root daemon.
 *
 * Owns the event loop and the transport, nothing else:
 *   - main loop: select() over netlink uevents, logdr socket, timerfd
 *   - netlink KOBJECT_UEVENT: power_supply/fb0/lcd -> dispatch
 *   - logdr stream "tail=1 lids=2": raw events -> notify.ev_parse()
 *   - adaptive timerfd: retune_timer() re-armed on every transition
 *   - signal test hooks
 *
 * All policy lives in plugins under mods/. The core only walks the
 * chgd_modes registry for armed "mode" packages (e.g. ring) and calls
 * the generic armed/idle branches otherwise.
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

#define RETRY_SEC    3     /* logdr reconnect backoff */
#define WATCHDOG_SEC 300   /* idle safety refresh (missed uevents) */

int g_tfd = -1;
int g_ld  = -1;

/* ---------------- mode registry access ---------------- */

const struct led_mode *mode_owns(const char *pkg)
{
    if (!pkg || !pkg[0]) return NULL;
    const struct led_mode *m;
    for (m = __start_chgd_modes; m < __stop_chgd_modes; m++)
        if (m->owns(pkg)) return m;
    return NULL;
}

/* ---------------- adaptive timer ---------------- */

/* One timerfd serves all pending timeouts; it is re-armed on every state
 * transition instead of ticking blindly every 30s:
 *   mode armed (ring rainbow) -> mode heartbeat (32ms)
 *   notification armed        -> 1s screen watch (notif_max_sec cap,
 *                                0 = unlimited)
 *   dialer ping seen          -> 2s missed-call verification window
 *   logdr down                -> RETRY_SEC reconnect backoff
 *   idle, all links up        -> WATCHDOG_SEC safety refresh only */
void retune_timer(void)
{
    if (g_tfd < 0) return;
    const char *why;
    long ms;
    const struct led_mode *m = mode_owns(g_st.cur_pkg);
    if (m)                             { ms = m->tick_ms;         why = m->name; }
    else if (g_st.cur_pkg[0])          { ms = 1000L;              why = "armed"; }
    else if (g_call_checks_left > 0)   { ms = CALL_RECHECK_MS;    why = "call check"; }
    else if (g_ld < 0)                 { ms = RETRY_SEC * 1000L;  why = "logdr retry"; }
    else                               { ms = WATCHDOG_SEC*1000L; why = "watchdog"; }
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
        ev_load_tags();
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
            handle_enqueue(&g_st, dialer_pkg_id());
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
                        !ring_is_active() &&
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
                    handle_enqueue(&g_st, pkg);
            }
        }

        /* --- adaptive housekeeping --- */
        if (FD_ISSET(tfd, &rfds)) {
            uint64_t exp;
            ssize_t ig = read(tfd, &exp, sizeof(exp)); (void)ig;

            const struct led_mode *m = mode_owns(g_st.cur_pkg);
            if (m) {
                /* --- mode tick (ring rainbow etc.) --- */
                m->tick();
            } else {
                maybe_call_check();      /* pending verification window */

                if (g_st.cur_pkg[0]) {
                    LOGI("armed tick");
                    double age = difftime(time(NULL), g_st.armed_at);
                    long cap = conf_notif_max_sec();
                    if (cap > 0 && age >= cap)      /* 0 = unlimited */
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