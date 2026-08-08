#ifndef REMOTICA_SYSTEM_INFO_H
#define REMOTICA_SYSTEM_INFO_H

/*
 * system_info.h
 * ==============
 * Facts about the host machine rather than the printer: is Remotica
 * running as an installed systemd service, will it come back by itself
 * after a power cut, and how much room is left for uploaded gcode.
 *
 * Deliberately its own file rather than more of api_handlers.c. That
 * file is about the printer; this one is the ONLY place in the backend
 * that runs an external command, and one of those commands runs as root
 * via sudo. Keeping it small and separate means the whole privileged
 * surface is a file you can read top to bottom in one sitting.
 *
 * How the privilege works, since it is not obvious from here: the
 * installed service runs as an unprivileged "remotica" user, and
 * packaging/install.sh drops a sudoers file permitting that user
 * exactly three systemctl command lines against exactly this unit —
 * nothing else, no wildcards, no password. So a bug anywhere in this
 * backend still cannot become root; the worst it can reach is
 * enabling or disabling this one service.
 */

#include <stddef.h>

/* Where packaging/install.sh puts the systemd unit. Its presence is what
 * "is this an installed service?" means throughout the backend. */
#define REMOTICA_UNIT_PATH "/etc/systemd/system/remotica.service"

/* Baked in by the Makefile from `git describe`. Falls back so a plain
 * `make` in a tarball with no .git still compiles. */
#ifndef REMOTICA_VERSION
#define REMOTICA_VERSION "dev"
#endif

/* 1 when the systemd unit file exists, meaning this is an installed
 * service whose boot behaviour can be changed. 0 in a source checkout,
 * where there is nothing to toggle. */
int system_info_is_managed(void);

/* 1 enabled, 0 disabled, -1 if it could not be determined. */
int system_info_boot_start_enabled(void);

/* Enables or disables start-on-boot. Returns 1 on success, or 0 on
 * failure with up to err_size-1 bytes of the command's own output copied
 * into `err` — callers are expected to surface that text rather than
 * swallow it, since a sudoers rule that doesn't match fails in a way
 * that is otherwise invisible from the UI. */
int system_info_set_boot_start(int enable, char *err, size_t err_size);

/* Free bytes on the filesystem holding `path`, or -1 if it can't be
 * determined. */
long long system_info_free_bytes(const char *path);

#endif /* REMOTICA_SYSTEM_INFO_H */
