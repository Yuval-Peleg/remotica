#ifndef REMOTICA_TRANSPORT_SIM_H
#define REMOTICA_TRANSPORT_SIM_H

/*
 * transport_sim.h
 * ================
 * The simulated printer driver — see transport.h for what a "driver" is
 * and why this exists. This one doesn't talk to any real hardware; it
 * just makes the shared PrinterState behave the way a real printer
 * roughly would (temperatures drift toward their target instead of
 * jumping instantly, etc).
 *
 * It also synthesizes the same gcode text a real printer conversation
 * would have (see console_log.h) — even though there's no real serial
 * port involved, generating and logging "G91" / "G1 X5.0 F3000" / "G90"
 * for a jog (using the same feed rates as transport_serial.c, see
 * JOG_FEED_RATE_* in transport.h) plus a synthesized "ok" reply makes the
 * frontend's console/terminal view show something meaningful during
 * development, and makes it obvious exactly what a real printer would
 * have received for the same action.
 *
 * This is what runs by default, so the whole system (frontend + backend)
 * is testable end to end without owning a real 3D printer.
 */

#include "printer_state.h"
#include "transport.h"

struct ConsoleLog;

/* Creates a new simulated driver bound to the given shared state, logging
 * synthesized gcode to `console` (see console_log.h) — pass NULL if you
 * don't want that. The returned pointer is heap-allocated — pass it to
 * transport_sim_destroy() when you're done with it (normally: never,
 * until the program exits). */
PrinterDriver *transport_sim_create(PrinterState *state, struct ConsoleLog *console);

/* Frees a driver created by transport_sim_create(). */
void transport_sim_destroy(PrinterDriver *driver);

#endif /* REMOTICA_TRANSPORT_SIM_H */
