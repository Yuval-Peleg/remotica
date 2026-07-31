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
 * How to use this instead of the simulator: run the backend with
 * `--serial /dev/ttyUSB0` (or whatever your printer's device path is —
 * see main.c). Without that flag, the simulator (transport_sim.h) is used
 * instead, which is the safe default.
 */

#include "printer_state.h"
#include "transport.h"

struct ConsoleLog;

/* Creates a new serial driver bound to the given shared state, for a
 * printer expected to be at `device_path` (e.g. "/dev/ttyUSB0" on Linux).
 * Every line sent to and received from the printer is recorded to
 * `console` (see console_log.h) as it happens — pass NULL if you don't
 * want that (e.g. in a test that doesn't care about logging).
 * This does NOT open the connection yet — that happens when
 * driver->connect() is called, same as the simulator, so both drivers
 * behave the same way from main.c's point of view. Returns NULL if
 * device_path is too long to store. */
PrinterDriver *transport_serial_create(PrinterState *state, struct ConsoleLog *console,
                                       const char *device_path);

/* Disconnects (if still connected) and frees a driver created by
 * transport_serial_create(). */
void transport_serial_destroy(PrinterDriver *driver);

#endif /* REMOTICA_TRANSPORT_SERIAL_H */
