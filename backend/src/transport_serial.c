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
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "console_log.h"

/* The baud rates this driver knows how to try. Just one for now: 115200,
 * the standard rate for essentially every 8-bit Marlin board and this
 * driver's only baud rate since it was first written. Some newer 32-bit
 * boards default to 250000 instead, which would be worth adding here —
 * but there's no standard POSIX Bxxxx constant for it (checked: Linux's
 * own termios headers go from B230400 straight to B460800), so
 * supporting it needs Linux's separate termios2/BOTHER ioctl mechanism
 * for arbitrary custom rates, which isn't implemented here yet. Rather
 * than list a baud rate that would silently fail to actually configure
 * the port, this table only contains rates that are genuinely supported.
 * `baud` is the plain human number (what gets logged, compared, and
 * passed around); `termios_speed` is the opaque B-constant termios
 * itself wants — not the literal numeric baud rate, a separately encoded
 * value. Both transport_serial_create() and transport_serial_discover()
 * use this same table, so they can never disagree about what baud rates
 * exist. */
static const struct {
    long baud;
    speed_t termios_speed;
} BAUD_RATE_OPTIONS[] = {
    {115200, B115200},
};

/* How long to wait for "ok" after different kinds of commands. Homing can
 * take a long time (the printer physically has to reach its limit
 * switches), so it gets a much longer allowance than a quick temperature
 * command.
 *
 * TIMEOUT_MS_LONG_RUNNING exists because a handful of gcode commands
 * legitimately don't answer for minutes, not seconds: M109/M190 don't
 * reply until the hotend/bed has actually REACHED its target (a cold
 * start is easily 2-5 minutes), G28/G29 physically home and probe the
 * bed, G4 is an explicit dwell, and M600 waits for a human to swap
 * filament. Real slicer start-gcode contains several of these, so
 * applying the 5-second allowance to them would abort virtually every
 * real print during its own warm-up. See command_timeout_ms() below. */
#define TIMEOUT_MS_NORMAL 5000
#define TIMEOUT_MS_HOMING 30000
#define TIMEOUT_MS_LONG_RUNNING 300000

/* The absolute ceiling on how long ONE command's wait can be stretched
 * by busy-keepalives (see is_busy_line() and the deadline helpers
 * below). Without a ceiling, "reset the deadline every time the printer
 * says it's still working" is an unbounded wait by construction, and an
 * unbounded wait here is exactly what the Deadlines comment block warns
 * about: it would hang job_manager_shutdown()'s pthread_join and leave
 * even Ctrl+C unable to stop the backend. 30 minutes is far past any
 * plausible single command (the longest realistic one, a full G29 mesh
 * probe on a large bed, is minutes) while still guaranteeing the wait
 * terminates. */
#define TIMEOUT_MS_BUSY_CAP 1800000

/* M115 gets its own, longer timeout — measured directly against a real
 * Ender 3 (Marlin 2.1.2.4): sending M115 to receiving its final "ok"
 * took ~7.6 SECONDS, not the ~5ms you'd expect for a one-line reply.
 * Marlin 2.x's M115 reply includes an extended capability report (a
 * "Cap:" line for every optional feature the firmware was built with —
 * ~40 of them on this board) on top of the actual FIRMWARE_NAME/
 * MACHINE_TYPE line this driver actually wants, and composing/sending
 * all of that apparently isn't instant. TIMEOUT_MS_NORMAL (5s) was cutting
 * the read off just before "ok" arrived, which is why discovery (and a
 * real connect, using the same query) reported a real, physically
 * connected, correctly-responding printer as "not found" — confirmed by
 * capturing the raw bytes independently of this driver and timing them.
 * 15s leaves real margin above the ~7.6s actually observed. */
#define TIMEOUT_MS_FIRMWARE_INFO 15000

/* How long a discovery probe waits for a reply from one candidate device
 * at one candidate baud rate before giving up and moving on. Tied to
 * TIMEOUT_MS_FIRMWARE_INFO — the same query, and the same budget, a real
 * connection gives its own M115 — so "discovery found it" reliably
 * implies "a real connect would too." A slower scan across multiple
 * WRONG candidates is the correct tradeoff for not missing the RIGHT
 * one, especially since in practice there's usually only one or two
 * candidate devices to try. */
#define DISCOVERY_PROBE_TIMEOUT_MS TIMEOUT_MS_FIRMWARE_INFO

/* How many times to resend a single line before giving up on it — see
 * send_checksummed_line() below. This driver never has more than one
 * line outstanding at a time (it always waits for a reply before sending
 * the next), so "resend" only ever means "send this exact same line
 * again," never "replay the last N lines." */
#define MAX_RESEND_ATTEMPTS 5

/* Arduino-compatible boards (which is what most 3D printer mainboards
 * are) reset themselves when a serial connection is opened — this is a
 * side effect of the DTR signal toggling, not something we control. After
 * a reset, the firmware takes a few seconds to boot up before it's ready
 * to accept commands — stock Creality boards in particular tend toward
 * the slower end of that (LCD splash screen, SD card init) before the
 * serial command parser is actually listening. We wait this long after
 * opening before treating the connection as ready. */
#define BOOT_WAIT_SECONDS 3

/* Only ask the printer for a fresh temperature reading every N ticks
 * (main.c ticks roughly every 300ms), so we're not spamming M105 far
 * faster than necessary — about once every ~1.8 seconds. */
#define TEMP_POLL_EVERY_N_TICKS 6

/* This driver's private data — only the functions in this file ever look
 * inside this struct (see the `void *impl_data` comment in transport.h). */
typedef struct {
    char device_path[64];
    long baud_rate; /* plain human number, e.g. 115200 — see BAUD_RATE_OPTIONS */
    int fd;         /* the open serial port, or -1 if not connected */

    /* A serial port is a single, strictly sequential conversation: one
     * command out, one "ok" back, and nothing may interleave in between.
     * But the driver functions below are called from THREE different
     * threads — civetweb's HTTP worker threads (jog/home/temp), the
     * shared 300ms tick thread (the M105 temperature poll), and the print
     * streamer thread in job_manager.c (send_gcode_line). Without this
     * lock, two writes can splice into one garbled line on the wire, and
     * two readers (read_line() consumes one byte at a time) can each end
     * up holding half of the same reply and neither ever sees a coherent
     * "ok". So every write-then-wait-for-ok exchange holds this for the
     * WHOLE exchange, not just the write.
     *
     * Lock ordering rule, followed everywhere in this file: io_lock is
     * taken first and released BEFORE printer_state_lock() is taken.
     * The two are never held at the same time, in either order — that's
     * what keeps a slow serial exchange from blocking the state
     * broadcaster, and makes a lock-order-inversion deadlock impossible
     * by construction. */
    pthread_mutex_t io_lock;

    int ticks_since_poll; /* counts up to TEMP_POLL_EVERY_N_TICKS */

    /* Line-number + checksum protocol state — see the big comment on
     * send_checksummed_line() for what this is and why. Both fields are
     * only ever touched while io_lock is held (set once at connect time,
     * read/advanced by every send_gcode_line call), same as everything
     * else in this struct. */
    int checksums_enabled; /* did the printer accept our M110 N0 at connect? */
    long next_line_number; /* the N value the next checksummed line will use */

    /* Set when a checksummed line failed without a confirmed "ok", which
     * leaves next_line_number pointing at a number the firmware may
     * already have seen. The next send re-negotiates M110 before sending
     * anything — see resync_line_numbers() for the (safety-relevant)
     * reason this can't just be ignored. */
    int needs_line_number_resync;

    /* If this driver was created via transport_serial_create_from_
     * discovery(), `fd` already holds an open, booted, past-its-M115-
     * query connection at creation time (instead of the usual -1), and
     * this holds the firmware info that query already captured — read
     * once by serial_connect() on the "already open" path and otherwise
     * unused. Empty string for a driver created the normal way. */
    char pending_firmware_info[256];

    /* True for a driver created without a fixed device path (--serial
     * auto with nothing found yet — see transport_serial_create_auto())
     * or one created from a successful auto-discovery (transport_serial_
     * create_from_discovery()) — either way, "auto" was requested. When
     * set, serial_connect() re-scans for a device on every call where fd
     * is still -1, instead of only ever trying a fixed device_path. This
     * is what lets main.c's reconnect thread pick up a printer that
     * wasn't plugged in yet at startup. False for an explicit --serial
     * <device>, which always retries that exact path and nothing else. */
    int auto_discover;
} SerialImplData;

/* ---------------------------------------------------------------------
 * Deadlines
 *
 * Every wait in this file is bounded by a single absolute deadline
 * computed once, up front, rather than by a per-read timeout. The
 * difference matters: Marlin can emit an unbounded number of non-"ok"
 * lines while we're waiting (auto temperature reports, "busy: processing"
 * keepalives), and a per-read timeout restarts the clock on every one of
 * them — so if the real "ok" is ever lost, the wait never ends, which
 * also hangs job_manager_shutdown()'s pthread_join and makes even Ctrl+C
 * unable to stop the backend. CLOCK_MONOTONIC (not CLOCK_REALTIME)
 * because it can't jump backwards if the system clock is adjusted.
 * --------------------------------------------------------------------- */

static void deadline_init(struct timespec *deadline, int timeout_ms) {
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec += 1;
    }
}

/* Milliseconds left until `deadline`, never negative — 0 means "already
 * expired", which poll() treats as "check once and return immediately",
 * exactly the behaviour we want. */
static int deadline_remaining_ms(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long long remaining = (long long)(deadline->tv_sec - now.tv_sec) * 1000LL +
                          (long long)(deadline->tv_nsec - now.tv_nsec) / 1000000LL;

    if (remaining < 0) {
        return 0;
    }
    if (remaining > INT_MAX) {
        return INT_MAX;
    }
    return (int)remaining;
}

/* True if `a` falls strictly later than `b`. */
static int deadline_is_after(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) {
        return a->tv_sec > b->tv_sec;
    }
    return a->tv_nsec > b->tv_nsec;
}

/* Restarts `deadline` at timeout_ms from NOW — but never past `cap`.
 *
 * This is the one deliberate exception to "computed once, up front"
 * above, and it exists for exactly one reason: a busy-keepalive (see
 * is_busy_line) is not chatter, it's the firmware explicitly telling us
 * "I am still executing the command you sent, do not give up on me". A
 * command that is still legitimately running has not consumed its
 * allowance in any meaningful sense, so it gets it back.
 *
 * Note this is NOT the same case as M115's capability dump, which the
 * comment above is written about: that's a bounded, known-length burst
 * of reply lines with a single "ok" at the end, so a shared budget is
 * the right model for it. A keepalive carries new information (progress
 * is being made) that a "Cap:" line does not. Resetting on it is also
 * what other RepRap hosts do — OctoPrint and Printrun both treat a busy
 * line as a reason to keep waiting rather than as an idle tick. `cap`
 * is what keeps this bounded regardless; see TIMEOUT_MS_BUSY_CAP. */
static void deadline_extend(struct timespec *deadline, int timeout_ms, const struct timespec *cap) {
    struct timespec extended;
    deadline_init(&extended, timeout_ms);
    *deadline = deadline_is_after(&extended, cap) ? *cap : extended;
}

/* ---------------------------------------------------------------------
 * Low-level line I/O
 * --------------------------------------------------------------------- */

/* Marlin reports genuinely bad news (thermal runaway, a failed
 * thermistor, "Printer halted. kill() called!") as lines beginning with
 * "Error:" or "!!". Those look like any other non-"ok" line to the wait
 * loops below, so without this they'd be discarded in silence while the
 * printer is on fire — literally, in the thermal-runaway case. This
 * doesn't attempt any recovery (that's a much bigger design question),
 * it just makes sure the line reaches the backend's own log; the raw text
 * is separately already visible in the frontend's gcode console, since
 * read_line() records everything it receives. */
static int is_error_line(const char *line) {
    return strncmp(line, "Error:", 6) == 0 || strncmp(line, "!!", 2) == 0;
}

static void note_error_line(const char *line) {
    fprintf(stderr, "Printer reported an error: %s\n", line);
}

/* True if `line` is Marlin's host-keepalive "I'm still working on the
 * last command" notice. Marlin emits "echo:busy: processing" roughly
 * every 2 seconds (HOST_KEEPALIVE_FEATURE, on by default) for as long as
 * a command is still executing — it's the firmware's own mechanism for
 * telling a host not to time out on it. The wait loops below reset their
 * deadline when they see one, via deadline_extend().
 *
 * Why this matters concretely (found against a real Ender 3, 2026-08-01):
 * an ordinary G0/G1 travel move right after G28 took just over 5s, past
 * TIMEOUT_MS_NORMAL, so a perfectly healthy printer that was busy-
 * keepaliving the whole time got treated as a driver failure and the
 * whole print was aborted after essentially one move.
 *
 * Matched with a plain case-sensitive prefix compare, same as
 * is_error_line above — Marlin prefixes host-facing notices with
 * "echo:", but the bare "busy:" spelling is accepted too since not every
 * fork/RepRap-family firmware includes the echo prefix.
 *
 * ONLY "processing" counts, deliberately. Marlin's other busy states are
 * "paused for user" and "paused for input" (M0/M1, M600 filament change,
 * a "continue?" prompt on the LCD), and those keep repeating for as long
 * as a HUMAN takes to walk over and press the knob — i.e. potentially
 * forever. Extending the deadline on those would make an unattended
 * printer able to block the streamer thread, and with it
 * job_manager_shutdown(), indefinitely. Those keep the ordinary fixed
 * timeout for their command (M600 is already in command_timeout_ms's
 * long-running list). */
static int is_busy_line(const char *line) {
    if (strncmp(line, "echo:", 5) == 0) {
        line += 5;
    }
    return strncmp(line, "busy: processing", 16) == 0;
}

/* Writes `line` followed by a newline to the serial port. write() is
 * allowed to write fewer bytes than asked in a single call (this is
 * normal, not an error), so this loops until everything has actually
 * gone out. Returns 0 on success, -1 on failure.
 *
 * If `console` is non-NULL, the line is also recorded there as a "sent"
 * entry — this is the actual, real gcode this driver sends, not a
 * simulation of it, so the frontend's terminal view shows exactly what
 * went out over the wire. */
static int write_line(int fd, const char *line, ConsoleLog *console) {
    /* 320, not 256: send_checksummed_line() below wraps a real gcode line
     * as "N<n> <line>*<checksum>", adding up to ~14 bytes of overhead
     * (an 8-digit line number is enough for a print with tens of millions
     * of lines in it). Sized so that wrapping never turns an
     * already-fits-in-256 real gcode line into one that no longer fits
     * here. */
    char buffer[320];
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

    if (console != NULL) {
        console_log_append(console, CONSOLE_DIRECTION_SENT, line);
    }

    return 0;
}

/* Reads a single line (up to and including the terminating '\n', which is
 * stripped) from the serial port, giving up at `deadline` (see the
 * deadline helpers above — the budget is shared with whatever else the
 * caller still has to do, it is NOT restarted per line).
 * Returns the number of bytes read into `buf` (not counting the removed
 * newline) on success, or -1 if the deadline was reached or an error
 * happened first. `buf` is always null-terminated on success.
 *
 * This reads one byte at a time, which is not the most efficient way to
 * do serial I/O, but printer replies are short (a few dozen bytes) and
 * infrequent, so simplicity and correctness matter a lot more here than
 * squeezing out extra performance.
 *
 * A line too long for `buf` is reported as a failure, but the rest of it
 * is still read and thrown away first: leaving the tail sitting in the
 * OS receive buffer would make it look like the beginning of the NEXT
 * line, and one misaligned line is all it takes to leave an "ok"
 * unconsumed and desync the whole protocol from then on.
 *
 * If `console` is non-NULL and a complete line is actually read (not on
 * timeout/error), it's recorded there as a "received" entry. */
static int read_line(int fd, char *buf, size_t buf_size, const struct timespec *deadline,
                     ConsoleLog *console) {
    size_t length = 0;
    int overflowed = 0;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    for (;;) {
        int poll_result = poll(&pfd, 1, deadline_remaining_ms(deadline));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (poll_result == 0) {
            return -1; /* deadline hit with no complete line received */
        }

        char byte;
        ssize_t n = read(fd, &byte, 1);
        if (n <= 0) {
            return -1; /* port closed or a real read error */
        }

        if (byte == '\n') {
            if (overflowed) {
                return -1; /* line was longer than our buffer, now fully discarded */
            }
            buf[length] = '\0';
            if (console != NULL) {
                console_log_append(console, CONSOLE_DIRECTION_RECEIVED, buf);
            }
            return (int)length;
        }
        if (byte == '\r') {
            continue; /* skip carriage returns, keep everything else */
        }
        if (length + 1 < buf_size) {
            buf[length++] = byte;
        } else {
            overflowed = 1; /* keep draining to the newline, see above */
        }
    }
}

/* Sends a command and waits for a reply line starting with "ok" (Marlin's
 * standard "I processed that, send me the next one" acknowledgement),
 * for at most timeout_ms in TOTAL across however many lines arrive first.
 * Reply lines that DON'T start with "ok" (unsolicited temperature
 * auto-reports, "busy: processing" keepalives) are ignored — this
 * function is only used for commands where we don't need to look at the
 * reply's contents, just confirm it succeeded. Returns 0 on success, -1
 * on failure or timeout.
 *
 * The tcflush() on the failure path is important and not just tidiness:
 * if we give up on a command and its "ok" then turns up a moment later,
 * the NEXT command's wait would be satisfied by that stale ack instead of
 * its own. From that point on this driver runs permanently one command
 * ahead of the printer, believing moves have completed that haven't —
 * silent, and exactly the kind of desync that ends with a nozzle
 * somewhere it shouldn't be. Dropping whatever is in the input buffer is
 * the cheap way to guarantee we resynchronise. */
static int send_and_wait_for_ok(int fd, const char *line, int timeout_ms, ConsoleLog *console) {
    if (write_line(fd, line, console) != 0) {
        return -1;
    }

    struct timespec deadline;
    struct timespec busy_cap;
    deadline_init(&deadline, timeout_ms);
    deadline_init(&busy_cap, TIMEOUT_MS_BUSY_CAP);

    char reply[256];
    while (read_line(fd, reply, sizeof(reply), &deadline, console) >= 0) {
        if (strncmp(reply, "ok", 2) == 0) {
            return 0;
        }
        if (is_busy_line(reply)) {
            /* Still executing — give the command its full allowance back
             * rather than counting the time it spent working against it.
             * See deadline_extend() and is_busy_line(). */
            deadline_extend(&deadline, timeout_ms, &busy_cap);
            continue;
        }
        if (is_error_line(reply)) {
            note_error_line(reply);
        }
        /* Not an "ok" line — keep waiting for one until the deadline. */
    }

    tcflush(fd, TCIFLUSH);
    return -1;
}

/* RepRap's standard line checksum: XOR of every byte in the line
 * (everything from the start up to, but not including, the "*"). This
 * isn't Marlin-specific — it's from the original RepRap serial protocol,
 * and it's implemented the same way by Prusa firmware, RepRapFirmware
 * (Duet boards), Smoothieware, Repetier-Firmware, and Klipper's host
 * software (which supports it specifically for compatibility with hosts
 * that always send it, which is most of them). */
static unsigned char gcode_checksum(const char *s) {
    unsigned char sum = 0;
    for (; *s != '\0'; s++) {
        sum ^= (unsigned char)*s;
    }
    return sum;
}

/* Re-runs the M110 N0 line-number negotiation mid-connection, so the
 * next checksummed line starts again from N1. Must be called with
 * io_lock held (every caller is already inside a send path that holds
 * it). Returns 0 on success, -1 if the printer didn't acknowledge.
 *
 * Why this exists — a real safety bug, found against a real Ender 3
 * (Marlin 2.1.2.4) on 2026-08-01:
 *
 * send_checksummed_line() only advances next_line_number after a
 * confirmed "ok". That's correct for a resend (the firmware wants the
 * SAME N again), but it means a line that failed outright leaves N
 * pointing at a number that was already put on the wire once. Marlin's
 * line-number protocol treats a repeat of an already-seen N as "you're
 * re-sending something I already have" and simply re-acks it WITHOUT
 * re-parsing the new content. So the next command sent — with different
 * text, but that stale N — gets a cheerful "ok" and is silently thrown
 * away.
 *
 * The command that lands in that slot is not arbitrary. A failed line
 * makes job_manager.c's streamer abort the print, and the very first
 * thing it does on abort is send_abort_safety_sequence(): "M104 S0",
 * "M140 S0", "M107". Those all went out under the stale number and were
 * all silently swallowed, while this driver reported success. Observed
 * live: hotend and bed sat at full print temperature for 20+ minutes
 * after an "aborted" print, with the backend believing it had shut them
 * down.
 *
 * The fix has to be a real resynchronisation, not just "advance N on
 * failure too": a failed line was never confirmed received at all, so
 * skipping past its number risks the opposite desync — the firmware
 * demanding a resend of a number this driver has already moved beyond.
 * M110 is RepRap's purpose-built answer to exactly this. It states what
 * N to expect on the NEXT line with no ambiguity about old content, it's
 * sent unnumbered (so it can't itself be swallowed as a duplicate), and
 * it's the same negotiation serial_connect() already does once at
 * connect time.
 *
 * Deliberately lazy — triggered from the next send rather than pushed
 * from job_manager.c. The stale number is a property of this driver's
 * protocol state, so keeping the repair inside the transport layer means
 * it protects EVERY later checksummed line: not just the abort-safety
 * sequence, but also the first line of the next print started on the
 * same connection, which would otherwise be silently dropped in exactly
 * the same way. It also keeps PrinterDriver's interface unchanged, so
 * transport_sim.c needs no matching concept it has no use for. */
static int resync_line_numbers(int fd, SerialImplData *impl, ConsoleLog *console) {
    /* Whatever is still sitting in the input buffer belongs to the
     * exchange that just failed — a late "ok" arriving now must not be
     * allowed to satisfy the M110 below. */
    tcflush(fd, TCIFLUSH);

    if (send_and_wait_for_ok(fd, "M110 N0", TIMEOUT_MS_NORMAL, console) != 0) {
        return -1;
    }

    impl->next_line_number = 1;
    impl->needs_line_number_resync = 0;
    return 0;
}

/* Sends `line` wrapped as "N<n> <line>*<checksum>" instead of bare, so
 * the printer can actually detect a corrupted transmission instead of
 * just executing whatever it received. Without this, a single flipped
 * bit (USB-serial running next to stepper motors and heaters is not a
 * quiet electrical environment) can turn one valid command into a
 * DIFFERENT, still-perfectly-valid-looking one — "G1 X100" corrupted
 * into "G1 X900" is not something Marlin has any way to notice on its
 * own. A checksum mismatch (or an out-of-sequence line number) instead
 * gets a "Resend: N<n>" reply, and this function resends the exact same
 * line — never a later one, since this driver never has more than one
 * line outstanding at a time — up to MAX_RESEND_ATTEMPTS times before
 * giving up. `impl->next_line_number` only advances on success, so a run
 * of resends keeps reusing the same N, which is exactly what the
 * firmware expects a resend to look like.
 *
 * Deliberately scoped to the print-streaming path only (see
 * serial_send_gcode_line) — not jog/home/temp. Those are already refused
 * outright while a print is running (api_handlers.c), so they're
 * low-volume, interactive, and any problem is immediately visible to
 * whoever just pressed the button; an unattended multi-hour print
 * streamed line by line with nobody watching is the case that actually
 * needs corruption detection. Mixing checksummed print lines with plain
 * jog/home/temp lines on the same connection is fine: Marlin only
 * applies line-number/checksum validation to lines that actually start
 * with "N" — anything else is processed normally regardless of whether
 * checksums were negotiated earlier in the connection.
 *
 * If a previous call failed outright (no "ok", no usable "Resend"), the
 * line numbering is no longer known to agree with the firmware's, so
 * this re-runs the M110 negotiation before sending anything — see
 * resync_line_numbers() for why that is a safety issue and not just
 * tidiness. */
static int send_checksummed_line(int fd, SerialImplData *impl, const char *line, int timeout_ms,
                                 ConsoleLog *console) {
    if (impl->needs_line_number_resync && resync_line_numbers(fd, impl, console) != 0) {
        return -1; /* printer isn't answering at all — nothing to be done */
    }

    for (int attempt = 0; attempt < MAX_RESEND_ATTEMPTS; attempt++) {
        char numbered[330];
        int numbered_len =
            snprintf(numbered, sizeof(numbered), "N%ld %s", impl->next_line_number, line);
        if (numbered_len < 0 || (size_t)numbered_len >= sizeof(numbered)) {
            return -1; /* line too long even before the checksum suffix */
        }

        char wrapped[340];
        int wrapped_len =
            snprintf(wrapped, sizeof(wrapped), "%s*%u", numbered, gcode_checksum(numbered));
        if (wrapped_len < 0 || (size_t)wrapped_len >= sizeof(wrapped)) {
            return -1;
        }

        if (write_line(fd, wrapped, console) != 0) {
            /* A partial write may have put half a numbered line on the
             * wire, so the firmware's idea of the sequence is now
             * anyone's guess. */
            impl->needs_line_number_resync = 1;
            return -1;
        }

        struct timespec deadline;
        struct timespec busy_cap;
        deadline_init(&deadline, timeout_ms);
        deadline_init(&busy_cap, TIMEOUT_MS_BUSY_CAP);

        int got_ok = 0;
        int got_resend = 0;
        char reply[256];
        while (read_line(fd, reply, sizeof(reply), &deadline, console) >= 0) {
            if (strncmp(reply, "ok", 2) == 0) {
                got_ok = 1;
                break;
            }
            if (is_busy_line(reply)) {
                /* Still executing this line — same reasoning as in
                 * send_and_wait_for_ok. This is the single most important
                 * place for it: every line of a real print goes through
                 * here, and print moves are exactly the commands that
                 * legitimately outrun TIMEOUT_MS_NORMAL. */
                deadline_extend(&deadline, timeout_ms, &busy_cap);
                continue;
            }
            /* Marlin's actual wording is "Resend: N<n>" — checked
             * case-insensitively since not every fork capitalizes it the
             * same way. We don't need to parse out <n>: with only one
             * line ever outstanding, whatever the firmware wants resent
             * can only be the line we just sent. */
            if (strncasecmp(reply, "resend", 6) == 0) {
                got_resend = 1;
                break;
            }
            if (is_error_line(reply)) {
                note_error_line(reply);
            }
        }

        if (got_ok) {
            impl->next_line_number++;
            return 0;
        }
        if (got_resend) {
            continue; /* retry the exact same line and line number */
        }

        /* Neither "ok" nor "Resend" within the deadline — same input-
         * buffer resync reasoning as send_and_wait_for_ok's failure
         * path, plus the line-numbering resync below. */
        tcflush(fd, TCIFLUSH);
        impl->needs_line_number_resync = 1;
        return -1;
    }

    /* Kept getting "Resend" until we ran out of attempts. The firmware
     * and this driver clearly don't agree about the sequence, so don't
     * let the next line assume they do. */
    impl->needs_line_number_resync = 1;
    return -1;
}

/* Sends M115 ("firmware info") and captures the most useful line of
 * whatever comes back before "ok" into `out` (left as an empty string if
 * the printer never replies with anything at all). This is a
 * BEST-EFFORT hint, not reliable identification — see the big comment on
 * PrinterState's firmware_info field for why.
 *
 * Originally this just kept the FIRST non-"ok" reply line, on the
 * assumption that would be the actual FIRMWARE_NAME:... info line — true
 * for a simple M115 reply, but a real Ender 3 (Marlin 2.1.2.4) sends an
 * unrelated "echo:SD card ok" line BEFORE the real M115 response, which
 * got captured instead of anything useful. Now specifically looks for a
 * line containing "FIRMWARE_NAME:" — the one thing every Marlin-family
 * M115 reply is expected to include — and only falls back to the first
 * line seen if no such line ever turns up, so firmware that formats its
 * reply differently still gets SOME hint rather than nothing.
 *
 * Also measured directly against that same Ender 3: a real M115 reply
 * can run to dozens of lines (Marlin 2.x's extended "Cap:" capability
 * report) and take several real seconds to fully arrive — this keeps
 * reading (and discarding anything not useful) until "ok" or
 * TIMEOUT_MS_FIRMWARE_INFO, not just the first line, so a slow/chatty
 * firmware doesn't leave leftover bytes sitting in the read buffer for
 * the next real command to trip over.
 *
 * Returns 1 if the printer replied with anything at all (even a bare
 * "ok" with no other lines), 0 if nothing came back before the deadline
 * — used by callers that need to know "is anything actually there",
 * separately from whatever ended up in `out`. */
static int query_firmware_info(int fd, ConsoleLog *console, char *out, size_t out_size) {
    out[0] = '\0';

    if (write_line(fd, "M115", console) != 0) {
        return 0;
    }

    struct timespec deadline;
    deadline_init(&deadline, TIMEOUT_MS_FIRMWARE_INFO);

    char reply[256];
    char first_line[256];
    first_line[0] = '\0';
    int got_any_reply = 0;

    while (read_line(fd, reply, sizeof(reply), &deadline, console) >= 0) {
        got_any_reply = 1;

        if (strncmp(reply, "ok", 2) == 0) {
            if (out[0] == '\0' && first_line[0] != '\0') {
                snprintf(out, out_size, "%s", first_line);
            }
            return 1;
        }
        if (is_error_line(reply)) {
            note_error_line(reply);
        }
        if (first_line[0] == '\0') {
            snprintf(first_line, sizeof(first_line), "%s", reply);
        }
        /* snprintf (not strncpy) specifically because its destination
         * size is a runtime parameter here, not a compile-time
         * sizeof(out) the compiler can see — strncpy's silent, possibly-
         * unterminated truncation is exactly the pattern
         * -Wstringop-truncation warns about in that situation, even
         * though it would've been safe here too. */
        if (out[0] == '\0' && strstr(reply, "FIRMWARE_NAME:") != NULL) {
            snprintf(out, out_size, "%s", reply);
        }
    }

    /* Deadline hit without ever seeing "ok" — same fallback as above, and
     * same resync reasoning as send_and_wait_for_ok's failure path: we're
     * leaving without having consumed M115's "ok" (if anything arrived at
     * all), so drop anything still in flight rather than let it ack
     * somebody else's command later. */
    if (out[0] == '\0' && first_line[0] != '\0') {
        snprintf(out, out_size, "%s", first_line);
    }
    tcflush(fd, TCIFLUSH);
    return got_any_reply;
}

/* Looks up the termios speed_t constant for a human baud number (e.g.
 * 115200), falling back to the more common of the two if `baud` isn't
 * one we recognize — should never actually happen, since every caller
 * gets `baud` from BAUD_RATE_OPTIONS in the first place, but a function
 * that returns a speed_t has to return SOMETHING. */
static speed_t baud_to_termios_speed(long baud) {
    for (size_t i = 0; i < sizeof(BAUD_RATE_OPTIONS) / sizeof(BAUD_RATE_OPTIONS[0]); i++) {
        if (BAUD_RATE_OPTIONS[i].baud == baud) {
            return BAUD_RATE_OPTIONS[i].termios_speed;
        }
    }
    return B115200;
}

/* Opens `device_path` and configures it as a raw serial port at `baud`.
 * Shared by serial_connect() (the real, final connection) and
 * transport_serial_discover()'s probing below — both need exactly the
 * same open/termios dance, just for different reasons. On success writes
 * the new fd to *out_fd and returns 0; on any failure returns -1 having
 * already closed anything it opened, so callers never have to clean up
 * a partially-configured fd themselves. */
static int open_and_configure(const char *device_path, long baud, int *out_fd) {
    /* O_NOCTTY: don't let this serial port become our process's
     * "controlling terminal" (it isn't one, and letting the OS treat it
     * like one can cause weird signal-related surprises).
     * O_NONBLOCK here during open() just avoids open() itself blocking
     * while waiting for a carrier-detect signal that serial-over-USB
     * adapters don't really use — we immediately switch to the blocking
     * mode we actually want below. */
    int fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        /* Logged here (not left for the caller) specifically because an
         * open() failure is worth knowing about even during discovery's
         * scan of several candidates — unlike "no reply within the
         * timeout" (an expected, silent outcome for every candidate that
         * just isn't a printer), a failure here almost always means
         * something actionable: most commonly EACCES because the
         * device's group ownership (typically "dialout" on Linux) does
         * not include the current user. Without this, that looked
         * identical to "nothing plugged in" — a real printer, physically
         * connected, was silently treated the same as no printer at all,
         * with no way to tell the two apart from the output. */
        fprintf(stderr, "  could not open %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    /* Switch back to normal blocking reads/writes now that the port is
     * open — our own read_line()/write_line() manage timeouts themselves
     * using poll(), so we don't want O_NONBLOCK interfering with that. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios options;
    if (tcgetattr(fd, &options) != 0) {
        close(fd);
        return -1;
    }

    speed_t termios_speed = baud_to_termios_speed(baud);
    cfsetispeed(&options, termios_speed);
    cfsetospeed(&options, termios_speed);

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
        close(fd);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

/* Tries `device_path` at `baud`: opens and configures it, waits out the
 * usual boot reset, and queries firmware info (reusing
 * query_firmware_info() rather than a separate hand-rolled M115
 * exchange, so probing and a real connect's firmware query can never
 * drift out of sync with each other).
 *
 * On success (something replied), the fd is left OPEN — not closed —
 * and handed back via *out_fd, along with whatever firmware info came
 * back via out_firmware_info. That's deliberate: this exact, already-
 * open, already-past-its-boot-reset-and-M115-wait connection gets handed
 * straight to transport_serial_create_from_discovery() by the caller, on
 * the winning candidate, so the real connection doesn't pay for a SECOND
 * DTR-triggered reset and a second multi-second M115 wait on top of the
 * one discovery already did — roughly halving how long --serial auto
 * takes end to end compared to probing, throwing the connection away,
 * and reconnecting from scratch.
 *
 * On failure, the fd is closed before returning, so the caller never has
 * to worry about cleanup either way. Returns 1 on success, 0 otherwise
 * (wrong device, wrong baud, or nothing plugged in at all). */
static int probe_device(const char *device_path, long baud, ConsoleLog *console, int *out_fd,
                        char *out_firmware_info, size_t firmware_info_size) {
    int fd;
    if (open_and_configure(device_path, baud, &fd) != 0) {
        return 0;
    }

    sleep(BOOT_WAIT_SECONDS);
    tcflush(fd, TCIOFLUSH);

    if (!query_firmware_info(fd, console, out_firmware_info, firmware_info_size)) {
        close(fd);
        return 0;
    }

    *out_fd = fd;
    return 1;
}

int transport_serial_discover(ConsoleLog *console, SerialDiscoveryResult *out) {
    static const char *const globs[] = {"/dev/ttyACM*", "/dev/ttyUSB*"};

    for (size_t g = 0; g < sizeof(globs) / sizeof(globs[0]); g++) {
        /* Zero-initialized so that globfree() below is always safe to
         * call, even if glob() fails (GLOB_NOMATCH or otherwise) without
         * touching gl_pathc/gl_pathv at all — neither POSIX nor glibc's
         * own man page promises those fields are left in any particular
         * state on failure, so relying on that would risk globfree()
         * later free()ing garbage stack memory. gl_pathc = 0 and
         * gl_pathv = NULL makes it a safe no-op regardless. */
        glob_t matches = {0};
        if (glob(globs[g], 0, NULL, &matches) != 0) {
            globfree(&matches);
            continue; /* no matches for this pattern, or glob() itself failed — either way, next */
        }

        for (size_t i = 0; i < matches.gl_pathc; i++) {
            const char *path = matches.gl_pathv[i];

            for (size_t b = 0; b < sizeof(BAUD_RATE_OPTIONS) / sizeof(BAUD_RATE_OPTIONS[0]); b++) {
                long baud = BAUD_RATE_OPTIONS[b].baud;
                printf("  trying %s at %ld baud...\n", path, baud);
                fflush(stdout);

                int fd;
                if (probe_device(path, baud, console, &fd, out->firmware_info,
                                 sizeof(out->firmware_info))) {
                    snprintf(out->device_path, sizeof(out->device_path), "%s", path);
                    out->baud_rate = baud;
                    out->fd = fd;
                    globfree(&matches);
                    return 1;
                }
            }
        }

        globfree(&matches);
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * PrinterDriver function implementations
 * --------------------------------------------------------------------- */

static int serial_connect(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    /* Queried into a local buffer first (not directly into the shared
     * state) because it involves real serial I/O with multi-second
     * timeouts — same "don't hold the lock during slow work" reasoning
     * as printer_state_to_json(). */
    char firmware_info[sizeof(((PrinterState *)0)->firmware_info)];

    /* Local, not impl->fd, until we actually have something to publish —
     * see the auto_discover branch below for why. Starts at impl->fd:
     * that's only ever already >= 0 here if this driver was created via
     * transport_serial_create_from_discovery(), i.e. this is the very
     * first connect() call after a successful startup scan. */
    int fd = impl->fd;

    if (fd < 0 && impl->auto_discover) {
        /* No device known yet — either transport_serial_create_auto()
         * was used because nothing was found at startup, or a previous
         * connect() (this one or an earlier reconnect attempt) found
         * nothing either. (Re)scan now: this is what lets a driver
         * created before any printer was plugged in pick one up later,
         * since main.c's reconnect thread just keeps calling connect()
         * again on a timer. Deliberately done before io_lock is taken —
         * same as open_and_configure() below — so a concurrent jog/home
         * request against a still-disconnected driver keeps failing fast
         * (impl->fd is still -1) instead of blocking for the whole scan. */
        SerialDiscoveryResult discovered;
        if (!transport_serial_discover(self->console, &discovered)) {
            return -1; /* still nothing out there */
        }
        snprintf(impl->device_path, sizeof(impl->device_path), "%s", discovered.device_path);
        impl->baud_rate = discovered.baud_rate;
        fd = discovered.fd;
        snprintf(impl->pending_firmware_info, sizeof(impl->pending_firmware_info), "%s",
                 discovered.firmware_info);
    }

    if (fd >= 0) {
        /* Already open and past its boot-reset wait and M115 query —
         * either pre-opened at construction time via transport_serial_
         * create_from_discovery(), or just found by the scan right
         * above. Reusing it here, instead of closing it and opening a
         * fresh connection from scratch, skips a second DTR-triggered
         * reset and a second multi-second M115 wait, which is the whole
         * point: it roughly halves how long --serial auto takes end to
         * end. */
        snprintf(firmware_info, sizeof(firmware_info), "%s", impl->pending_firmware_info);
        pthread_mutex_lock(&impl->io_lock);
        impl->fd = fd;
    } else {
        if (open_and_configure(impl->device_path, impl->baud_rate, &fd) != 0) {
            fprintf(stderr, "Failed to open/configure serial port %s: %s\n", impl->device_path,
                    strerror(errno));
            return -1;
        }

        pthread_mutex_lock(&impl->io_lock);
        impl->fd = fd;

        /* See the BOOT_WAIT_SECONDS comment above: most printer
         * mainboards reset when the port opens, so give the firmware
         * time to finish booting before we start sending it commands. */
        sleep(BOOT_WAIT_SECONDS);

        /* Discard whatever the board said while it was booting. This has
         * to happen AFTER the sleep, not before it: the reset is
         * triggered by our own open() a moment ago, so at this point in
         * the function the boot banner ("start", version strings,
         * SD-card chatter) hasn't been sent yet — flushing first would
         * flush an empty buffer and leave all of it to be misread as
         * replies to our first real command. */
        tcflush(fd, TCIOFLUSH);

        query_firmware_info(fd, self->console, firmware_info, sizeof(firmware_info));
    }

    /* M110 N0 tells the firmware "the next line I send will be N1" — the
     * standard RepRap way to (re)synchronise line numbering before using
     * checksummed lines (see send_checksummed_line). This is a NEGOTIATION,
     * not an assumption: firmware that doesn't understand line numbers/
     * checksums at all will fail to answer this cleanly (an error, a
     * "Resend", or just a timeout, since M110 itself isn't checksummed
     * here and firmware in checksum-required-mode would ask us to resend
     * it) rather than a plain "ok". Falling back to checksums_enabled = 0
     * in that case is exactly today's behaviour — bare, unnumbered
     * lines — so a firmware that doesn't support this protocol is no
     * worse off than before this feature existed, and only a firmware
     * that does support it (which is most of the Marlin-descended and
     * RepRap-protocol family — see gcode_checksum's comment) actually
     * gets the added corruption detection. */
    impl->checksums_enabled =
        (send_and_wait_for_ok(impl->fd, "M110 N0", TIMEOUT_MS_NORMAL, self->console) == 0);
    impl->next_line_number = 1;
    impl->needs_line_number_resync = 0; /* just negotiated — numbering is known-good */

    pthread_mutex_unlock(&impl->io_lock);

    printer_state_lock(self->state);
    self->state->connected = 1;
    snprintf(self->state->firmware_info, sizeof(self->state->firmware_info), "%s", firmware_info);
    printer_state_unlock(self->state);

    return 0;
}

static void serial_disconnect(PrinterDriver *self) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    /* Under io_lock so the fd can't be closed while another thread is
     * part-way through a write/read on it — main.c stops civetweb and
     * joins the print streamer before calling this, so in practice
     * nothing else should still be in here, but closing an fd out from
     * under a concurrent read() is bad enough (the number can be reused
     * by an unrelated open()) to be worth making structurally
     * impossible rather than merely unlikely. */
    pthread_mutex_lock(&impl->io_lock);
    if (impl->fd >= 0) {
        close(impl->fd);
        impl->fd = -1;
    }
    pthread_mutex_unlock(&impl->io_lock);

    printer_state_lock(self->state);
    self->state->connected = 0;
    printer_state_unlock(self->state);
}

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
     * driver (and most gcode files) assume as the normal mode.
     *
     * All three lines go out under one io_lock hold: they're a single
     * modal-state transaction, and another thread slipping a command in
     * between the G91 and the G90 would have it silently reinterpreted as
     * a relative move. (api_handlers.c also refuses jogs outright while a
     * print is running, for exactly the same reason at a higher level.) */
    pthread_mutex_lock(&impl->io_lock);
    if (impl->fd < 0) {
        pthread_mutex_unlock(&impl->io_lock);
        return -1; /* not connected */
    }

    int failed = 0;
    if (send_and_wait_for_ok(impl->fd, "G91", TIMEOUT_MS_NORMAL, self->console) != 0) {
        failed = 1;
    } else if (send_and_wait_for_ok(impl->fd, move_command, TIMEOUT_MS_NORMAL, self->console) !=
               0) {
        /* The move failed, but we're the ones who put the printer into
         * relative mode, so still try to put it back — leaving it in G91
         * would silently change the meaning of every later command. */
        failed = 1;
        send_and_wait_for_ok(impl->fd, "G90", TIMEOUT_MS_NORMAL, self->console);
    } else if (send_and_wait_for_ok(impl->fd, "G90", TIMEOUT_MS_NORMAL, self->console) != 0) {
        failed = 1;
    }
    pthread_mutex_unlock(&impl->io_lock);

    if (failed) {
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
    pthread_mutex_lock(&impl->io_lock);
    if (impl->fd < 0) {
        pthread_mutex_unlock(&impl->io_lock);
        return -1; /* not connected */
    }
    int result = send_and_wait_for_ok(impl->fd, "G28", TIMEOUT_MS_HOMING, self->console);
    pthread_mutex_unlock(&impl->io_lock);

    if (result != 0) {
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

    pthread_mutex_lock(&impl->io_lock);
    if (impl->fd < 0) {
        pthread_mutex_unlock(&impl->io_lock);
        return -1; /* not connected */
    }
    int result = send_and_wait_for_ok(impl->fd, command, TIMEOUT_MS_NORMAL, self->console);
    pthread_mutex_unlock(&impl->io_lock);

    if (result != 0) {
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

    /* trylock, NOT lock: this runs on the shared tick thread that also
     * drives the WebSocket broadcast, and the port can legitimately be
     * held for minutes at a time by a print streaming an M109 "wait for
     * temperature" line. Blocking here would stall every connected
     * browser's live view for that entire wait, to fetch a temperature
     * reading that is only ever nice-to-have. Skipping the poll and
     * retrying on the next tick costs nothing. ticks_since_poll is
     * deliberately left at (or above) the threshold when we skip, so we
     * retry every tick until the port frees up rather than waiting
     * another full poll interval. */
    if (pthread_mutex_trylock(&impl->io_lock) != 0) {
        return;
    }
    impl->ticks_since_poll = 0;

    /* M105 asks the printer to report its current temperatures. The
     * reply looks something like:
     *   ok T:205.32 /210.00 B:59.81 /60.00 @:87 B@:32
     * "T:" is the hotend (current /target), "B:" is the bed. We don't
     * assume "ok" and the temperature data are on the same line or in any
     * particular order relative to each other across firmware versions,
     * so this sends M105, then reads reply lines until it finds one
     * containing "T:" (which is where the useful data lives).
     *
     * Note that it then keeps reading until the "ok" rather than
     * returning the moment the numbers are parsed: with Marlin's
     * temperature auto-reporting enabled (M155), a SPONTANEOUS report can
     * arrive before our M105's own reply, and returning early there would
     * leave the real "ok" in the buffer to be mistaken for the
     * acknowledgement of whatever command goes out next. */
    if (write_line(impl->fd, "M105", self->console) != 0) {
        pthread_mutex_unlock(&impl->io_lock);
        return;
    }

    struct timespec deadline;
    deadline_init(&deadline, TIMEOUT_MS_NORMAL);

    int got_temps = 0;
    int saw_ok = 0;
    double hotend_current = 0.0, hotend_target = 0.0, bed_current = 0.0, bed_target = 0.0;

    char reply[256];
    while (read_line(impl->fd, reply, sizeof(reply), &deadline, self->console) >= 0) {
        if (is_error_line(reply)) {
            note_error_line(reply);
        }

        /* Find "T:" and "B:" anywhere in the line, then parse the two
         * numbers that follow each one. Searching with strstr() first
         * (rather than trying to do it all in one sscanf format string)
         * avoids a subtle scanf edge case: a %[^T]-style "skip until T"
         * conversion technically requires at least one non-T character
         * before it, so it can misbehave if a line happens to start with
         * "T:" itself. Doing the search explicitly sidesteps that. */
        char *hotend_part = strstr(reply, "T:");
        char *bed_part = strstr(reply, "B:");

        if (!got_temps && hotend_part != NULL && bed_part != NULL) {
            int hotend_matched = sscanf(hotend_part, "T:%lf /%lf", &hotend_current, &hotend_target);
            int bed_matched = sscanf(bed_part, "B:%lf /%lf", &bed_current, &bed_target);
            got_temps = (hotend_matched == 2 && bed_matched == 2);
        }

        if (strncmp(reply, "ok", 2) == 0) {
            saw_ok = 1;
            break;
        }
    }

    if (!saw_ok) {
        tcflush(impl->fd, TCIFLUSH); /* same resync reasoning as send_and_wait_for_ok */
    }
    pthread_mutex_unlock(&impl->io_lock);

    /* State is only touched after io_lock is released — see the lock
     * ordering rule on SerialImplData. */
    if (got_temps) {
        printer_state_lock(self->state);
        self->state->hotend.current_c = hotend_current;
        self->state->hotend.target_c = hotend_target;
        self->state->bed.current_c = bed_current;
        self->state->bed.target_c = bed_target;
        printer_state_unlock(self->state);
    }
}

/* How long to allow for `line`'s "ok", based on its command word — see
 * the TIMEOUT_MS_LONG_RUNNING comment at the top of the file for why a
 * handful of commands need minutes rather than seconds.
 *
 * The comparison is against the whole command word (everything up to the
 * first space), not a prefix: "G4" is a dwell, but "G40" is not, and a
 * plain strncmp("G4", ...) would treat both the same. */
static int command_timeout_ms(const char *line) {
    static const char *const long_running[] = {"M109", "M190", "G28", "G29", "M600", "G4"};

    size_t word_len = 0;
    while (line[word_len] != '\0' && line[word_len] != ' ' && line[word_len] != '\t') {
        word_len++;
    }

    for (size_t i = 0; i < sizeof(long_running) / sizeof(long_running[0]); i++) {
        if (strlen(long_running[i]) == word_len && strncmp(line, long_running[i], word_len) == 0) {
            return TIMEOUT_MS_LONG_RUNNING;
        }
    }
    return TIMEOUT_MS_NORMAL;
}

/* Real hardware doesn't get an optimistic position update the way
 * jog/home do above — a queued print file can contain thousands of
 * arbitrary G0/G1 moves in either absolute or relative mode (tracking
 * that correctly would mean re-implementing a chunk of Marlin's own
 * modal-state logic), and there's no reliable, firmware-agnostic way to
 * just ask the printer "where are you now". So position/extruder
 * readouts in the UI will lag reality during a real print — a known
 * limitation, not a bug. */
static int serial_send_gcode_line(PrinterDriver *self, const char *line) {
    SerialImplData *impl = (SerialImplData *)self->impl_data;

    pthread_mutex_lock(&impl->io_lock);
    if (impl->fd < 0) {
        pthread_mutex_unlock(&impl->io_lock);
        return -1; /* not connected */
    }
    int timeout_ms = command_timeout_ms(line);
    int result = impl->checksums_enabled
                     ? send_checksummed_line(impl->fd, impl, line, timeout_ms, self->console)
                     : send_and_wait_for_ok(impl->fd, line, timeout_ms, self->console);
    pthread_mutex_unlock(&impl->io_lock);

    return result;
}

/* Shared setup between transport_serial_create(), transport_serial_
 * create_from_discovery(), and transport_serial_create_auto() — they
 * differ only in what impl->fd, impl->pending_firmware_info, and
 * impl->auto_discover start as, which each caller sets itself afterward.
 * Returns NULL (having freed anything already allocated) on failure,
 * same as all three public functions. */
static PrinterDriver *alloc_serial_driver(PrinterState *state, ConsoleLog *console,
                                          const char *device_path, long baud_rate) {
    if (strlen(device_path) >= sizeof(((SerialImplData *)0)->device_path)) {
        return NULL; /* device path too long for our fixed-size buffer */
    }

    SerialImplData *impl = malloc(sizeof(SerialImplData));
    if (impl == NULL) {
        return NULL;
    }
    strncpy(impl->device_path, device_path, sizeof(impl->device_path) - 1);
    impl->device_path[sizeof(impl->device_path) - 1] = '\0';
    impl->baud_rate = baud_rate;
    impl->fd = -1;
    impl->ticks_since_poll = 0;
    impl->checksums_enabled = 0; /* real value decided by serial_connect's M110 negotiation */
    impl->next_line_number = 1;
    impl->needs_line_number_resync = 0;
    impl->pending_firmware_info[0] = '\0';
    impl->auto_discover = 0; /* the two callers that want this override it below */
    pthread_mutex_init(&impl->io_lock, NULL);

    PrinterDriver *driver = malloc(sizeof(PrinterDriver));
    if (driver == NULL) {
        pthread_mutex_destroy(&impl->io_lock);
        free(impl);
        return NULL;
    }

    driver->connect = serial_connect;
    driver->disconnect = serial_disconnect;
    driver->jog = serial_jog;
    driver->home = serial_home;
    driver->set_target_temp = serial_set_target_temp;
    driver->tick = serial_tick;
    driver->send_gcode_line = serial_send_gcode_line;
    driver->impl_data = impl;
    driver->state = state;
    driver->console = console;

    return driver;
}

PrinterDriver *transport_serial_create(PrinterState *state, ConsoleLog *console,
                                       const char *device_path, long baud_rate) {
    return alloc_serial_driver(state, console, device_path, baud_rate);
}

PrinterDriver *transport_serial_create_from_discovery(PrinterState *state, ConsoleLog *console,
                                                      const char *device_path, long baud_rate,
                                                      int fd, const char *firmware_info) {
    PrinterDriver *driver = alloc_serial_driver(state, console, device_path, baud_rate);
    if (driver == NULL) {
        /* Documented to always take ownership of fd, success or not. */
        close(fd);
        return NULL;
    }

    SerialImplData *impl = (SerialImplData *)driver->impl_data;
    impl->fd = fd;
    snprintf(impl->pending_firmware_info, sizeof(impl->pending_firmware_info), "%s", firmware_info);
    /* Only ever called from the --serial auto path, so if this driver is
     * ever disconnected and needs to reconnect later, it should re-scan
     * rather than assume the printer comes back on this exact path. */
    impl->auto_discover = 1;

    return driver;
}

PrinterDriver *transport_serial_create_auto(PrinterState *state, ConsoleLog *console) {
    PrinterDriver *driver = alloc_serial_driver(state, console, "", SERIAL_BAUD_RATE_DEFAULT);
    if (driver == NULL) {
        return NULL;
    }

    ((SerialImplData *)driver->impl_data)->auto_discover = 1;

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
        /* Only ever called from main.c's shutdown path, after civetweb
         * has stopped and the print streamer has been joined, so nothing
         * can still be holding this. */
        pthread_mutex_destroy(&impl->io_lock);
        free(impl);
    }

    free(driver);
}
