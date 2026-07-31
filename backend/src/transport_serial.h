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

/* Disconnects (if still connected) and frees a driver created by
 * transport_serial_create(). */
void transport_serial_destroy(PrinterDriver *driver);

/* What device path and baud rate transport_serial_discover() found. */
typedef struct {
    char device_path[64];
    long baud_rate;
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

#endif /* REMOTICA_TRANSPORT_SERIAL_H */
