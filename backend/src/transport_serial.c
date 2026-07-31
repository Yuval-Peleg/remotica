/*
 * transport_serial.c
 * ====================
 * See transport_serial.h for the overview and the important warning about
 * this being untested against real hardware. This file is the "how":
 * opening a serial port correctly, speaking G-code over it, and parsing
 * the printer's replies.
 *
 * Background for anyone not familiar with G-code / serial printer comms:
 * A 3D printer's firmware (Marlin is the most common, and what this code
 * assumes) reads text commands one line at a time over the serial
 * connection, e.g. "G1 X10 F3000\n" means "move to X=10mm at 3000mm/min".
 * After the firmware finishes handling a line, it replies with a line
 * that starts with "ok" — that's the signal "I'm ready for the next
 * command". Sending commands without waiting for "ok" can overflow the
 * firmware's tiny internal buffer and cause commands to be dropped, so
 * this code always waits for "ok" (with a timeout) before sending the
 * next line.
 */

/* Building with -std=c99 tells glibc to only expose strict C99 (plus a
 * couple of little else) by default, which hides several POSIX/BSD
 * functions this file needs (cfmakeraw() in particular). _DEFAULT_SOURCE
 * asks glibc to expose its normal, full feature set on top of C99 — it
 * must be defined before any system header is included, which is why
 * this is the very first thing in the file. */
#define _DEFAULT_SOURCE

#include "transport_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Most Marlin-based printers default to 115200 baud. Some (especially
 * newer 32-bit boards) use 250000 instead — if this doesn't work with a
 * particular printer, that's the first thing to check and change. */
#define SERIAL_BAUD_RATE B115200

/* How long to wait for "ok" after different kinds of commands. Homing can
 * take a long time (the printer physically has to reach its limit
 * switches), so it gets a much longer allowance than a quick temperature
 * command. */
#define TIMEOUT_MS_NORMAL 5000
#define TIMEOUT_MS_HOMING 30000

/* Arduino-compatible boards (which is what most 3D printer mainboards
 * are) reset themselves when a serial connection is opened — this is a
 * side effect of the DTR signal toggling, not something we control. After
 * a reset, the firmware takes a couple of seconds to boot up before it's
 * ready to accept commands. We wait this long after opening before
 * treating the connection as ready. */
#define BOOT_WAIT_SECONDS 2

/* Only ask the printer for a fresh temperature reading every N ticks
 * (main.c ticks roughly every 300ms), so we're not spamming M105 far
 * faster than necessary — about once every ~1.8 seconds. */
#define TEMP_POLL_EVERY_N_TICKS 6

/* This driver's private data — only the functions in this file ever look
 * inside this struct (see the `void *impl_data` comment in transport.h). */
typedef struct {
    char device_path[64];
    int fd;               /* the open serial port, or -1 if not connected */
    int ticks_since_poll; /* counts up to TEMP_POLL_EVERY_N_TICKS */
} SerialImplData;

/* ---------------------------------------------------------------------
 * Low-level line I/O
 * --------------------------------------------------------------------- */

/* Writes `line` followed by a newline to the serial port. write() is
 * allowed to write fewer bytes than asked in a single call (this is
 * normal, not an error), so this loops until everything has actually
 * gone out. Returns 0 on success, -1 on failure. */
static int write_line(int fd, const char *line) {
    char buffer[256];
    int written = snprintf(buffer, sizeof(buffer), "%s\n", line);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1; /* line too long for our buffer */
    }

    size_t total_sent = 0;
    while (total_sent < (size_t)written) {
        ssize_t sent = write(fd, buffer + total_sent, (size_t)written - total_sent);
        if (sent < 0) {
            if (errno == EINTR) {
                continue; /* interrupted by a signal, just retry */
            }
            return -1;
        }
        total_sent += (size_t)sent;
    }
    return 0;
}

/* Reads a single line (up to and including the terminating '\n', which is
 * stripped) from the serial port, waiting up to timeout_ms in total.
 * Returns the number of bytes read into `buf` (not counting the removed
 * newline) on success, or -1 if the timeout was reached or an error
 * happened first. `buf` is always null-terminated on success.
 *
 * This reads one byte at a time, which is not the most efficient way to
 * do serial I/O, but printer replies are short (a few dozen bytes) and
 * infrequent, so simplicity and correctness matter a lot more here than
 * squeezing out extra performance. */
static int read_line(int fd, char *buf, size_t buf_size, int timeout_ms) {
    size_t length = 0;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    while (length + 1 < buf_size) {
        int remaining_ms = timeout_ms; /* recomputing an exact remaining
                                        * budget adds complexity for
                                        * little real benefit here, since
                                        * printer replies normally arrive
                                        * as one small burst — each
                                        * individual poll() just reuses
                                        * the original timeout */
        int poll_result = poll(&pfd, 1, remaining_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (poll_result == 0) {
            return -1; /* timed out with no complete line received */
        }

        char byte;
        ssize_t n = read(fd, &byte, 1);
        if (n <= 0) {
            return -1; /* port closed or a real read error */
        }

        if (byte == '\n') {
            buf[length] = '\0';
            return (int)length;
        }
        if (byte != '\r') { /* skip carriage returns, keep everything else */
            buf[length++] = byte;
        }
    }

    return -1; /* line longer than our buffer — treat as a failure */
}

/* Sends a command and waits for a reply line starting with "ok" (Marlin's
 * standard "I processed that, send me the next one" acknowledgement).
 * Any reply lines that DON'T start with "ok" while waiting (e.g.
 * unsolicited temperature auto-reports) are simply ignored here — this
 * function is only used for commands where we don't need to look at the
 * reply's contents, just confirm it succeeded. Returns 0 on success, -1
 * on failure or timeout. */
static int send_and_wait_for_ok(int fd, const char *line, int timeout_ms) {
    if (write_line(fd, line) != 0) {
        return -1;
    }

    char reply[256];
    while (read_line(fd, reply, sizeof(reply), timeout_ms) >= 0) {
        if (strncmp(reply, "ok", 2) == 0) {
            return 0;
        }
        /* Not an "ok" line — keep waiting for one until we time out. */
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * PrinterDriver function implementations
 * --------------------------------------------------------------------- */

static int serial_connect(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    /* O_NOCTTY: don't let this serial port become our process's
     * "controlling terminal" (it isn't one, and letting the OS treat it
     * like one can cause weird signal-related surprises).
     * O_NONBLOCK here during open() just avoids open() itself blocking
     * while waiting for a carrier-detect signal that serial-over-USB
     * adapters don't really use — we immediately switch to the blocking
     * mode we actually want below. */
    int fd = open(impl->device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Failed to open serial port %s: %s\n", impl->device_path, strerror(errno));
        return -1;
    }

    /* Switch back to normal blocking reads/writes now that the port is
     * open — our own read_line()/write_line() manage timeouts themselves
     * using poll(), so we don't want O_NONBLOCK interfering with that. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios options;
    if (tcgetattr(fd, &options) != 0) {
        fprintf(stderr, "Failed to read serial port settings for %s: %s\n", impl->device_path,
                strerror(errno));
        close(fd);
        return -1;
    }

    cfsetispeed(&options, SERIAL_BAUD_RATE);
    cfsetospeed(&options, SERIAL_BAUD_RATE);

    /* "Raw mode": no line-editing, no character translation, no special
     * handling of control characters — we want exactly the bytes the
     * printer sends, unmodified. This is the standard way to configure a
     * serial port for talking to a device (as opposed to a human typing
     * at a terminal, which is what termios defaults assume). */
    cfmakeraw(&options);

    /* VMIN=0, VTIME=0 alongside blocking mode: return immediately with
     * whatever bytes are available, even zero. Combined with our own
     * poll()-based waiting in read_line(), this gives us full control
     * over timeouts instead of relying on termios' own (more limited)
     * timeout mechanism. */
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        fprintf(stderr, "Failed to configure serial port %s: %s\n", impl->device_path,
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Discard any leftover bytes sitting in the OS's serial buffers from
     * before we opened the port (e.g. boot-up noise). */
    tcflush(fd, TCIOFLUSH);

    impl->fd = fd;

    /* See the BOOT_WAIT_SECONDS comment above: most printer mainboards
     * reset when the port opens, so give the firmware time to finish
     * booting before we start sending it commands. */
    sleep(BOOT_WAIT_SECONDS);

    printer_state_lock(self->state);
    self->state->connected = 1;
    printer_state_unlock(self->state);

    return 0;
}

static void serial_disconnect(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    if (impl->fd >= 0) {
        close(impl->fd);
        impl->fd = -1;
    }

    printer_state_lock(self->state);
    self->state->connected = 0;
    printer_state_unlock(self->state);
}

/* Feed rates (in mm/minute, which is what G-code's F parameter expects)
 * used for manual jog moves. Z and E move much slower than X/Y on a
 * typical printer, so they get lower feed rates. These are conservative,
 * generally-safe defaults, not tuned for any specific printer. */
#define JOG_FEED_RATE_XY 3000.0
#define JOG_FEED_RATE_Z 600.0
#define JOG_FEED_RATE_E 300.0

static int serial_jog(PrinterDriver *self, char axis, double delta_mm) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    double feed_rate = JOG_FEED_RATE_XY;
    if (axis == 'Z') {
        feed_rate = JOG_FEED_RATE_Z;
    } else if (axis == 'E') {
        feed_rate = JOG_FEED_RATE_E;
    }

    char move_command[128];
    snprintf(move_command, sizeof(move_command), "G1 %c%.3f F%.0f", axis, delta_mm, feed_rate);

    /* G91 = relative positioning ("move BY this much", not "move TO this
     * position") — exactly what a jog command means. G90 switches back to
     * absolute positioning afterward, which is what the rest of this
     * driver (and most gcode files) assume as the normal mode. */
    if (send_and_wait_for_ok(impl->fd, "G91", TIMEOUT_MS_NORMAL) != 0) {
        return -1;
    }
    if (send_and_wait_for_ok(impl->fd, move_command, TIMEOUT_MS_NORMAL) != 0) {
        return -1;
    }
    if (send_and_wait_for_ok(impl->fd, "G90", TIMEOUT_MS_NORMAL) != 0) {
        return -1;
    }

    /* Marlin acknowledges a move once it's queued, not once the motor has
     * actually finished moving — but since we don't have a way to ask the
     * real printer "where are you right now" cheaply, we optimistically
     * update our shared state immediately, the same way the frontend's
     * mock and the simulator both do. If the move somehow fails partway
     * through on the real printer, this position will drift from
     * reality until the next G28 home resets it — a known limitation. */
    printer_state_lock(self->state);
    switch (axis) {
    case 'X':
        self->state->position.x_mm += delta_mm;
        break;
    case 'Y':
        self->state->position.y_mm += delta_mm;
        break;
    case 'Z':
        self->state->position.z_mm += delta_mm;
        break;
    case 'E':
        self->state->position.e_mm += delta_mm;
        break;
    default:
        break;
    }
    printer_state_unlock(self->state);

    return 0;
}

static int serial_home(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    /* G28 with no axis letters homes all of X, Y, and Z. */
    if (send_and_wait_for_ok(impl->fd, "G28", TIMEOUT_MS_HOMING) != 0) {
        return -1;
    }

    printer_state_lock(self->state);
    self->state->position.x_mm = 0.0;
    self->state->position.y_mm = 0.0;
    self->state->position.z_mm = 0.0;
    printer_state_unlock(self->state);

    return 0;
}

static int serial_set_target_temp(PrinterDriver *self, PrinterHeater heater, double celsius) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    /* M104 sets the hotend target WITHOUT waiting for it to be reached;
     * M140 does the same for the bed. (Their "wait for it" counterparts,
     * M109/M190, are deliberately NOT used here — blocking until a
     * target temperature is reached would stall this whole driver, and
     * therefore the whole backend, for potentially minutes.) */
    char command[32];
    if (heater == PRINTER_HEATER_HOTEND) {
        snprintf(command, sizeof(command), "M104 S%.1f", celsius);
    } else {
        snprintf(command, sizeof(command), "M140 S%.1f", celsius);
    }

    if (send_and_wait_for_ok(impl->fd, command, TIMEOUT_MS_NORMAL) != 0) {
        return -1;
    }

    printer_state_lock(self->state);
    if (heater == PRINTER_HEATER_HOTEND) {
        self->state->hotend.target_c = celsius;
    } else {
        self->state->bed.target_c = celsius;
    }
    printer_state_unlock(self->state);

    return 0;
}

static void serial_tick(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    if (impl->fd < 0) {
        return; /* not connected, nothing to poll */
    }

    impl->ticks_since_poll++;
    if (impl->ticks_since_poll < TEMP_POLL_EVERY_N_TICKS) {
        return;
    }
    impl->ticks_since_poll = 0;

    /* M105 asks the printer to report its current temperatures. The
     * reply looks something like:
     *   ok T:205.32 /210.00 B:59.81 /60.00 @:87 B@:32
     * "T:" is the hotend (current /target), "B:" is the bed. We don't
     * assume "ok" and the temperature data are on the same line or in any
     * particular order relative to each other across firmware versions,
     * so this sends M105, then reads reply lines until it either finds
     * one containing "T:" (which is where the useful data lives) or
     * times out. */
    if (write_line(impl->fd, "M105") != 0) {
        return;
    }

    char reply[256];
    while (read_line(impl->fd, reply, sizeof(reply), TIMEOUT_MS_NORMAL) >= 0) {
        /* Find "T:" and "B:" anywhere in the line, then parse the two
         * numbers that follow each one. Searching with strstr() first
         * (rather than trying to do it all in one sscanf format string)
         * avoids a subtle scanf edge case: a %[^T]-style "skip until T"
         * conversion technically requires at least one non-T character
         * before it, so it can misbehave if a line happens to start with
         * "T:" itself. Doing the search explicitly sidesteps that. */
        char *hotend_part = strstr(reply, "T:");
        char *bed_part = strstr(reply, "B:");

        if (hotend_part != NULL && bed_part != NULL) {
            double hotend_current, hotend_target, bed_current, bed_target;
            int hotend_matched = sscanf(hotend_part, "T:%lf /%lf", &hotend_current, &hotend_target);
            int bed_matched = sscanf(bed_part, "B:%lf /%lf", &bed_current, &bed_target);

            if (hotend_matched == 2 && bed_matched == 2) {
                printer_state_lock(self->state);
                self->state->hotend.current_c = hotend_current;
                self->state->hotend.target_c = hotend_target;
                self->state->bed.current_c = bed_current;
                self->state->bed.target_c = bed_target;
                printer_state_unlock(self->state);
                return;
            }
        }
        /* Not the line we're looking for (could be a bare "ok", or
         * something else) — keep reading until we find it or time out. */
    }
}

PrinterDriver *transport_serial_create(PrinterState *state, const char *device_path) {
    if (strlen(device_path) >= sizeof(((SerialImplData *)0)->device_path)) {
        return NULL; /* device path too long for our fixed-size buffer */
    }

    SerialImplData *impl = malloc(sizeof(SerialImplData));
    if (impl == NULL) {
        return NULL;
    }
    strncpy(impl->device_path, device_path, sizeof(impl->device_path) - 1);
    impl->device_path[sizeof(impl->device_path) - 1] = '\0';
    impl->fd = -1;
    impl->ticks_since_poll = 0;

    PrinterDriver *driver = malloc(sizeof(PrinterDriver));
    if (driver == NULL) {
        free(impl);
        return NULL;
    }

    driver->connect = serial_connect;
    driver->disconnect = serial_disconnect;
    driver->jog = serial_jog;
    driver->home = serial_home;
    driver->set_target_temp = serial_set_target_temp;
    driver->tick = serial_tick;
    driver->impl_data = impl;
    driver->state = state;

    return driver;
}

void transport_serial_destroy(PrinterDriver *driver) {
    if (driver == NULL) {
        return;
    }

    SerialImplData *impl = (SerialImplData *)driver->impl_data;
    if (impl != NULL) {
        if (impl->fd >= 0) {
            close(impl->fd);
        }
        free(impl);
    }

    free(driver);
}
