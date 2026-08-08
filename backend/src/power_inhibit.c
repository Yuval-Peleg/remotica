/* See the identical comment in main.c: -std=c99 hides POSIX functions
 * (fork, execv, kill here) unless glibc is asked for its normal feature
 * set before any system header. */
#define _DEFAULT_SOURCE

#include "power_inhibit.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Distro paths for the tool. Checked in order; the first executable one
 * wins. If none exists this whole file becomes a no-op — a non-systemd
 * machine has no logind to ask, and failing loudly on every tick would be
 * worse than saying so once. */
static const char *const INHIBIT_PATHS[] = {
    "/usr/bin/systemd-inhibit",
    "/bin/systemd-inhibit",
};

static pid_t s_child = -1;
static int s_unavailable_logged = 0;
/* Set when a lock was attempted and logind didn't have it. Stops this
 * retrying (and re-warning) every 300ms for the rest of the print. */
static int s_failed = 0;

static const char *find_inhibit_tool(void) {
    for (size_t i = 0; i < sizeof(INHIBIT_PATHS) / sizeof(INHIBIT_PATHS[0]); i++) {
        if (access(INHIBIT_PATHS[i], X_OK) == 0) {
            return INHIBIT_PATHS[i];
        }
    }
    return NULL;
}

/* Asks logind whether our lock actually exists. Returns 1 only on a
 * confirmed sighting.
 *
 * `systemd-inhibit --list` is the same view the user gets from a
 * terminal, which matters: when this disagrees with the dashboard, the
 * dashboard is what's wrong. Given a moment first, because the child has
 * to exec and register before it can appear. */
static int lock_registered_with_logind(void) {
    const char *tool = find_inhibit_tool();
    if (tool == NULL) {
        return 0;
    }

    usleep(400 * 1000);

    char command[256];
    snprintf(command, sizeof(command), "%s --list 2>/dev/null", tool);

    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        return 0;
    }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), pipe) != NULL) {
        if (strstr(line, "Remotica") != NULL) {
            found = 1;
        }
    }
    pclose(pipe);
    return found;
}

static void acquire(void) {
    const char *tool = find_inhibit_tool();
    if (tool == NULL) {
        if (!s_unavailable_logged) {
            printf("systemd-inhibit not found — cannot stop this machine suspending itself "
                   "mid-print. If it suspends, the print stops but the printer's heaters do "
                   "not. See the README on disabling idle suspend.\n");
            fflush(stdout);
            s_unavailable_logged = 1;
        }
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "power: fork failed, cannot block suspend: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Child. `sleep infinity` is the held process: systemd-inhibit
         * owns the lock for exactly as long as the command it runs, so
         * the lock lives until the parent kills this. */
        /* "idle", NOT "sleep:idle" — and this distinction is the whole
         * reason the first version of this silently did nothing.
         *
         * polkit's shipped defaults for logind differ between the two:
         *
         *   inhibit-block-sleep  allow_any = auth_admin_keep
         *   inhibit-block-idle   allow_any = yes
         *
         * "allow_any" is the branch that applies to a caller with no
         * login session — which is exactly what this service is, running
         * as a system user. So asking for the sleep half demanded
         * interactive admin authentication from a background daemon that
         * has no agent to answer it, the whole request was refused, and
         * systemd-inhibit sat there holding nothing while the machine
         * suspended mid-print.
         *
         * Blocking idle alone is also the correct scope, not just the
         * permitted one: the failure being prevented is the machine
         * dozing off on its own, and a human deliberately choosing
         * Suspend was always meant to be honoured. */
        char *const argv[] = {
            (char *)"systemd-inhibit", (char *)"--what=idle",
            (char *)"--who=Remotica",  (char *)"--why=A print is running",
            (char *)"--mode=block",    (char *)"sleep",
            (char *)"infinity",        NULL,
        };
        execv(tool, argv);
        _exit(127); /* exec failed — nothing useful left to do in the child */
    }

    s_child = pid;

    /* Forking successfully is NOT the same as holding the lock, and
     * treating it as such is how this went wrong once already: a machine
     * suspended mid-print while the dashboard cheerfully reported sleep
     * as blocked, because all this code actually knew was that it had
     * started a process. If systemd-inhibit can't reach logind, or is
     * refused, the child can sit there having registered nothing.
     *
     * So ask logind what it actually has. Anything other than a
     * confirmed lock is reported as not held — better a UI that admits
     * it can't protect the print than one that claims it can. */
    if (!lock_registered_with_logind()) {
        printf("WARNING: asked to block suspend for this print, but logind reports no such "
               "lock. This machine may still suspend mid-print, which stops the print but "
               "NOT the printer's heaters. Disable sleep instead — see the README.\n");
        fflush(stdout);
        kill(s_child, SIGTERM);
        waitpid(s_child, NULL, 0);
        s_child = -1;
        s_failed = 1;
        return;
    }

    s_failed = 0;
    printf("Blocking automatic suspend while the print runs (confirmed with logind).\n");
    fflush(stdout);
}

static void release(void) {
    if (s_child <= 0) {
        return;
    }

    kill(s_child, SIGTERM);
    /* Blocking wait, deliberately: the child is `sleep`, which dies on
     * SIGTERM immediately, and reaping it here is what stops a zombie
     * accumulating for every print. */
    waitpid(s_child, NULL, 0);
    s_child = -1;

    printf("Print finished — automatic suspend is allowed again.\n");
    fflush(stdout);
}

void power_inhibit_set(int active) {
    /* If the child died on its own (killed externally, systemd-inhibit
     * failing to exec), forget it so the next active tick can retry
     * rather than believing a lock is held that isn't. */
    if (s_child > 0 && waitpid(s_child, NULL, WNOHANG) == s_child) {
        s_child = -1;
    }

    if (active && s_child <= 0 && !s_failed) {
        acquire();
    } else if (!active) {
        release();
        s_failed = 0; /* a new print gets a fresh attempt */
    }
}

int power_inhibit_is_held(void) {
    return s_child > 0;
}

void power_inhibit_shutdown(void) {
    release();
}
