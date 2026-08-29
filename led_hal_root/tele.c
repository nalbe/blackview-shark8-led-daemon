/*
 * tele.c - one-shot child-process capture and telephony state probes.
 * Used by the call plugins; everything is event-triggered, no polling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include "chgd.h"

/* Fork/exec a command, capture stdout (capped), 5s hard timeout.
 * SIGCHLD is SIG_IGN so children are auto-reaped. Returns bytes captured. */
size_t run_capture(char *const argv[], char *out, size_t cap)
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
int tele_ringing(void)
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
int tele_active(void)
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