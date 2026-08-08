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

static const char *find_inhibit_tool(void) {
    for (size_t i = 0; i < sizeof(INHIBIT_PATHS) / sizeof(INHIBIT_PATHS[0]); i++) {
        if (access(INHIBIT_PATHS[i], X_OK) == 0) {
            return INHIBIT_PATHS[i];
        }
    }
    return NULL;
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
        char *const argv[] = {
            (char *)"systemd-inhibit", (char *)"--what=sleep:idle",
            (char *)"--who=Remotica",  (char *)"--why=A print is running",
            (char *)"--mode=block",    (char *)"sleep",
            (char *)"infinity",        NULL,
        };
        execv(tool, argv);
        _exit(127); /* exec failed — nothing useful left to do in the child */
    }

    s_child = pid;
    printf("Blocking automatic suspend while the print runs.\n");
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

    if (active && s_child <= 0) {
        acquire();
    } else if (!active && s_child > 0) {
        release();
    }
}

int power_inhibit_is_held(void) {
    return s_child > 0;
}

void power_inhibit_shutdown(void) {
    release();
}
