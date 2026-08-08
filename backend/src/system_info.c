/* Same reason as the identical line in main.c and transport_serial.c:
 * -std=c99 hides POSIX functions (popen/pclose here) unless glibc is
 * asked for its normal feature set before any system header. */
#define _DEFAULT_SOURCE

#include "system_info.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

int system_info_is_managed(void) {
    struct stat st;
    return stat(REMOTICA_UNIT_PATH, &st) == 0;
}

/* Runs one of a fixed set of commands and returns its exit status, with
 * up to err_size-1 bytes of its combined output copied into `err`.
 *
 * SAFETY — read before changing anything here. Every caller passes a
 * compile-time string constant. Nothing from the network is ever
 * interpolated into `cmd`: the boot-start endpoint picks between two
 * fixed strings on a boolean rather than building a command out of a
 * request body. That is what makes running this through a shell safe at
 * all. If a future change needs a variable in one of these commands,
 * it must switch to fork/execv with an argument vector instead of
 * formatting a string — do not add a %s here. */
static int run_fixed_command(const char *cmd, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) {
        return -1;
    }

    if (err != NULL && err_size > 0) {
        size_t used = 0;
        int c;
        while ((c = fgetc(pipe)) != EOF) {
            if (used < err_size - 1) {
                err[used++] = (char)c;
            }
        }
        err[used] = '\0';

        /* Trailing newline is noise once this is a JSON string or a
         * toast in the browser. */
        while (used > 0 && (err[used - 1] == '\n' || err[used - 1] == '\r')) {
            err[--used] = '\0';
        }
    }

    return pclose(pipe);
}

int system_info_boot_start_enabled(void) {
    if (!system_info_is_managed()) {
        return -1;
    }

    char out[64];
    run_fixed_command("sudo /usr/bin/systemctl is-enabled remotica.service 2>&1", out, sizeof(out));

    /* The exit status alone can't answer this: `is-enabled` exits
     * non-zero for a disabled unit, which is indistinguishable from the
     * command having failed outright. The word it prints is the actual
     * answer, so that's what we read. */
    if (strncmp(out, "enabled", 7) == 0) {
        return 1;
    }
    if (strncmp(out, "disabled", 8) == 0) {
        return 0;
    }
    return -1;
}

int system_info_set_boot_start(int enable, char *err, size_t err_size) {
    const char *cmd = enable ? "sudo /usr/bin/systemctl enable remotica.service 2>&1"
                             : "sudo /usr/bin/systemctl disable remotica.service 2>&1";
    return run_fixed_command(cmd, err, err_size) == 0;
}

long long system_info_free_bytes(const char *path) {
    /* The data directory is created lazily — on the first profile save
     * or upload — so on a fresh install it legitimately doesn't exist
     * yet when this is first asked. Walking up to the nearest existing
     * ancestor isn't a fudge: free space is a property of the
     * filesystem, and that's the same filesystem the directory will be
     * created on, so the answer is the one the caller wanted. Without
     * this, a brand new install reports "unknown" until someone happens
     * to save something. */
    char candidate[512];
    snprintf(candidate, sizeof(candidate), "%s", path);

    for (;;) {
        struct statvfs vfs;
        if (statvfs(candidate, &vfs) == 0) {
            return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
        }

        char *slash = strrchr(candidate, '/');
        if (slash == NULL) {
            /* A relative path with nothing left to strip ("data") means
             * the current directory is the filesystem to ask about. */
            return statvfs(".", &vfs) == 0 ? (long long)vfs.f_bavail * (long long)vfs.f_frsize : -1;
        }
        if (slash == candidate) {
            /* Stripped all the way to "/" and even that failed. */
            return -1;
        }
        *slash = '\0';
    }
}
