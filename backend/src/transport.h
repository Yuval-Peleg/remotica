#ifndef REMOTICA_TRANSPORT_H
#define REMOTICA_TRANSPORT_H

/*
 * transport.h
 * ============
 * This defines PrinterDriver: the "interface" (in C, since we don't have
 * classes, this means "a struct full of function pointers") that the REST
 * API talks to when it wants to actually DO something to the printer —
 * jog an axis, home, change a target temperature.
 *
 * Why an interface instead of just calling functions directly:
 * We have two very different ways of "talking to a printer":
 *   1. transport_sim.c — a fake printer that lives entirely in memory,
 *      used when there's no real hardware attached (e.g. right now, while
 *      developing). It answers instantly and makes up plausible-looking
 *      behavior (temperatures drifting toward target, etc).
 *   2. transport_serial.c — a REAL printer connected over USB, talked to
 *      using actual G-code over a serial port.
 * The REST handlers (api_handlers.c) shouldn't need to know or care which
 * one is active — they just want to say "jog X by 1mm" and have it
 * happen. So both transport_sim.c and transport_serial.c implement this
 * same set of functions, and whichever one main.c decided to create gets
 * used everywhere else through this shared PrinterDriver pointer. This is
 * the same idea as an "interface" in Java/TypeScript or a "trait" in
 * Rust — C just spells it out manually with function pointers.
 *
 * How a function-pointer struct works, concretely: PrinterDriver has
 * fields like `int (*jog)(...)` — that's a variable that holds the
 * ADDRESS of a function, not a function itself. transport_sim.c fills in
 * these fields with pointers to its own functions (like sim_jog), and
 * transport_serial.c fills them in with pointers to ITS functions (like
 * serial_jog). Calling `driver->jog(driver, 'X', 1.0)` then runs whichever
 * function was actually plugged in — that's how the same call site ends
 * up running different code depending on which driver is active.
 */

#include "printer_state.h"

/* Which heater a temperature command is targeting. */
typedef enum { PRINTER_HEATER_HOTEND, PRINTER_HEATER_BED } PrinterHeater;

typedef struct PrinterDriver {
    /* Opens the connection to the printer (or, for the simulator, just
     * marks it as "connected"). Returns 0 on success, -1 on failure. */
    int (*connect)(struct PrinterDriver *self);

    /* Closes the connection and releases any resources. Safe to call even
     * if connect() was never called or already failed. */
    void (*disconnect)(struct PrinterDriver *self);

    /* Move one axis ('X', 'Y', 'Z', or 'E') by delta_mm (can be negative).
     * Returns 0 on success, -1 on failure (e.g. not connected). */
    int (*jog)(struct PrinterDriver *self, char axis, double delta_mm);

    /* Home all axes (move to the printer's reference position, normally
     * X0 Y0 Z0). Returns 0 on success, -1 on failure. */
    int (*home)(struct PrinterDriver *self);

    /* Set a target temperature in Celsius for the hotend or the bed.
     * Returns 0 on success, -1 on failure. */
    int (*set_target_temp)(struct PrinterDriver *self, PrinterHeater heater, double celsius);

    /* Called regularly (see main.c's background tick loop, roughly every
     * 300ms) so the driver can do anything it needs to do on a timer:
     * the simulator uses this to drift current temperatures toward their
     * targets; the real serial driver uses this to poll the printer for
     * fresh temperature readings. */
    void (*tick)(struct PrinterDriver *self);

    /* Every driver needs somewhere to keep its own private data (e.g. the
     * serial driver needs to remember the file descriptor for the open
     * serial port). Each implementation defines its own small struct for
     * this and stores a pointer to it here — the rest of the codebase
     * never looks inside impl_data, only the driver's own functions do. */
    void *impl_data;

    /* The shared state this driver reads from and writes to. Set once
     * when the driver is created (see transport_sim_create /
     * transport_serial_create) and never changed after that. */
    PrinterState *state;
} PrinterDriver;

#endif /* REMOTICA_TRANSPORT_H */
