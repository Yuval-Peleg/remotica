#ifndef REMOTICA_TRANSPORT_SERIAL_H
#define REMOTICA_TRANSPORT_SERIAL_H

/*
 * transport_serial.h
 * ====================
 * The REAL printer driver — talks to an actual 3D printer over a USB
 * serial connection, using G-code, the same text-based command language
 * OctoPrint and every slicer's "print over USB" feature uses.
 *
 * *** IMPORTANT — READ BEFORE USING WITH A REAL PRINTER ***
 * This code has been written carefully and matches standard patterns for
 * talking to Marlin-family 3D printer firmware, but it has NOT been
 * tested against a real printer (there wasn't one available while writing
 * it). Before trusting it with real hardware — especially anything that
 * moves motors or drives heaters — it's worth a careful review (ideally
 * with a stronger model for a second pass, see the project's memory notes
 * on when to do that) and testing cautiously: watch the printer while it
 * runs the first few commands, keep a hand near the power switch, and
 * don't leave it unattended until you've seen it behave correctly.
 *
 * Threading: every PrinterDriver function this file provides is safe to
 * call from any thread, and they really are called from three of them
 * (civetweb's HTTP workers, the shared tick thread, and the print
 * streamer thread) — a single mutex inside the driver serialises the
 * whole send-command-then-wait-for-"ok" exchange, because a serial port
 * is one sequential conversation and two overlapping exchanges corrupt
 * each other. See the io_lock comment in transport_serial.c.
 *
 * Corruption detection: gcode lines streamed during a print are sent
 * with a line number and checksum (the standard RepRap protocol, not
 * Marlin-specific), so a bit flipped in transit gets caught and resent
 * instead of silently executed as a different, still-valid-looking
 * command. Negotiated automatically at connect time (via M110 N0) with a
 * clean fallback to plain unnumbered lines for firmware that doesn't
 * support it — see gcode_checksum()/send_checksummed_line() in
 * transport_serial.c.
 *
 * How to use this instead of the simulator: run the backend with
 * `--serial /dev/ttyUSB0` (or whatever your printer's device path is —
 * see main.c), or `--serial auto` to have it find the device itself (see
 * transport_serial_discover() below). Without either flag, the simulator
 * (transport_sim.h) is used instead, which is the safe default.
 */

#include "printer_state.h"
#include "transport.h"

struct ConsoleLog;

/* 115200 baud — the standard rate for essentially every 8-bit Marlin
 * board, and currently the only rate this driver actually supports (see
 * BAUD_RATE_OPTIONS in transport_serial.c for why: some newer 32-bit
 * boards use 250000 instead, but there's no standard way to configure
 * that rate without Linux-specific ioctls this driver doesn't implement
 * yet). Used both as transport_serial_create()'s default and as the rate
 * transport_serial_discover() tries per candidate device. */
#define SERIAL_BAUD_RATE_DEFAULT 115200L

/* Creates a new serial driver bound to the given shared state, for a
 * printer expected to be at `device_path` (e.g. "/dev/ttyUSB0" on Linux)
 * and talking at `baud_rate` (e.g. 115200 — see SERIAL_BAUD_RATE_DEFAULT
 * above; pass whatever transport_serial_discover() found if you used
 * that, or SERIAL_BAUD_RATE_DEFAULT for a normal explicit --serial
 * <device>). Every line sent to and received from the printer is
 * recorded to `console` (see console_log.h) as it happens — pass NULL if
 * you don't want that (e.g. in a test that doesn't care about logging).
 * This does NOT open the connection yet — that happens when
 * driver->connect() is called, same as the simulator, so both drivers
 * behave the same way from main.c's point of view. Returns NULL if
 * device_path is too long to store. */
PrinterDriver *transport_serial_create(PrinterState *state, struct ConsoleLog *console,
                                       const char *device_path, long baud_rate);

/* Disconnects (if still connected) and frees a driver created by either
 * transport_serial_create() or transport_serial_create_from_discovery(). */
void transport_serial_destroy(PrinterDriver *driver);

/* What transport_serial_discover() found: where it is, what baud rate
 * answered, and — since discovery already has to open the device, wait
 * out its boot reset, and query its firmware info just to confirm it's
 * really there — the already-open file descriptor and the firmware info
 * already captured along the way, so a real connect afterward doesn't
 * have to redo any of that (see transport_serial_create_from_discovery
 * below). `fd` is only meaningful/open when transport_serial_discover()
 * returned 1; whoever receives a successful result owns it and must
 * either hand it to transport_serial_create_from_discovery() or close()
 * it themselves. */
typedef struct {
    char device_path[64];
    long baud_rate;
    char firmware_info[256];
    int fd;
} SerialDiscoveryResult;

/* Scans /dev/ttyACM* and /dev/ttyUSB* (the usual places a 3D printer's
 * USB connection shows up on Linux, whether it's a USB-CDC-ACM device or
 * a separate USB-to-serial chip) and, for each one found, tries opening
 * it at each of the baud rates in BAUD_RATE_OPTIONS (see
 * transport_serial.c) and sending a firmware-info query. The first
 * device that replies with anything at all is returned as the answer —
 * this is a best-effort heuristic (the same basic approach OctoPrint and
 * the Arduino IDE use for their own board auto-detection), not proof
 * that what it found is definitely the intended printer, especially if
 * more than one serial device is plugged in at once. Only ever sends a
 * firmware-info QUERY to each candidate — never anything that moves a
 * motor or drives a heater — but note that simply OPENING a port at all
 * triggers the same DTR-toggle reset behaviour real hardware detection
 * always does on any Arduino-compatible board sitting there, printer or
 * not; that's a universal property of USB-serial boards, not something
 * specific to this code.
 *
 * Returns 1 and fills `*out` on success, 0 if nothing answered. */
int transport_serial_discover(struct ConsoleLog *console, SerialDiscoveryResult *out);

/* Same as transport_serial_create(), but for a device
 * transport_serial_discover() already opened, booted, and queried —
 * hands the driver that exact already-open fd and already-known
 * firmware info directly instead of opening a second, separate
 * connection from scratch. This is what makes --serial auto only pay
 * for ONE DTR-triggered reset and ONE multi-second M115 wait instead of
 * two (one during discovery, one again during the "real" connect) —
 * see the io_lock/fd comment on serial_connect() in transport_serial.c
 * for exactly how the reuse works.
 *
 * Takes ownership of `fd` unconditionally: on success it's used for the
 * driver's connection, and on failure (e.g. out of memory) it's closed
 * before returning — the caller must not touch or close(fd) themselves
 * either way, and must not pass the same fd to more than one create
 * call. Returns NULL if device_path is too long to store. */
PrinterDriver *transport_serial_create_from_discovery(PrinterState *state,
                                                      struct ConsoleLog *console,
                                                      const char *device_path, long baud_rate,
                                                      int fd, const char *firmware_info);

/* Creates a driver for `--serial auto` when the initial startup scan
 * (transport_serial_discover()) found nothing — there's no device path to
 * store yet. Unlike transport_serial_create(), this driver's connect()
 * doesn't just try to open a fixed path: every call (the failed one at
 * startup, and every retry main.c's reconnect loop makes afterward)
 * re-scans for a device from scratch (see transport_serial_discover()),
 * so it picks up a printer plugged in after the process already started,
 * on whatever device path it happens to enumerate as. Once a scan
 * succeeds, later reconnects (if the printer is ever disconnected again)
 * keep re-scanning rather than assuming it'll come back on the same
 * path. */
PrinterDriver *transport_serial_create_auto(PrinterState *state, struct ConsoleLog *console);

#endif /* REMOTICA_TRANSPORT_SERIAL_H */
