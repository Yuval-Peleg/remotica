#ifndef REMOTICA_TRANSPORT_SIM_H
#define REMOTICA_TRANSPORT_SIM_H

/*
 * transport_sim.h
 * ================
 * The simulated printer driver — see transport.h for what a "driver" is
 * and why this exists. This one doesn't talk to any real hardware; it
 * just makes the shared PrinterState behave the way a real printer
 * roughly would (temperatures drift toward their target instead of
 * jumping instantly, etc), the same idea as the frontend's
 * use-printer-temps.js mock hook, just living in the backend instead.
 *
 * This is what runs by default, so the whole system (frontend + backend)
 * is testable end to end without owning a real 3D printer.
 */

#include "printer_state.h"
#include "transport.h"

/* Creates a new simulated driver bound to the given shared state. The
 * returned pointer is heap-allocated — pass it to transport_sim_destroy()
 * when you're done with it (normally: never, until the program exits). */
PrinterDriver *transport_sim_create(PrinterState *state);

/* Frees a driver created by transport_sim_create(). */
void transport_sim_destroy(PrinterDriver *driver);

#endif /* REMOTICA_TRANSPORT_SIM_H */
