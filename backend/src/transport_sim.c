/*
 * transport_sim.c
 * ================
 * See transport_sim.h for the overview. This file fills in the
 * PrinterDriver function pointers with "pretend" behavior.
 */

#include "transport_sim.h"

#include <stdlib.h>

/* How many degrees the simulated temperature moves toward its target on
 * each tick. main.c calls tick() roughly every 300ms, so a hotend step of
 * 0.3 means it heats up at about 1 degree per second — a rough real-world
 * heating rate. The bed is set slower, since real print beds heat up more
 * slowly than hotends. */
#define HOTEND_STEP_PER_TICK 0.3
#define BED_STEP_PER_TICK 0.15

/* Moves `current` toward `target` by up to `step`, without overshooting
 * past it. This is the same "drift toward target" idea as the frontend's
 * use-printer-temps.js hook. */
static double step_toward(double current, double target, double step) {
    if (current < target) {
        double next = current + step;
        return (next > target) ? target : next;
    }
    if (current > target) {
        double next = current - step;
        return (next < target) ? target : next;
    }
    return current;
}

/* Adds a small amount of random jitter, so temperature readings don't
 * look robotically smooth (real sensors have noise). Returns a value in
 * [-amount, +amount].
 *
 * Note on thread-safety: rand() keeps its internal state in a global
 * variable, which would be a data race if called from multiple threads at
 * once. That's not a problem here because only main.c's single background
 * tick thread ever calls driver->tick() (see main.c) — REST handlers
 * never call tick() themselves. If that ever changes, this would need
 * rand_r() with its own seed per thread instead. */
static double jitter(double amount) {
    double fraction = (double)rand() / (double)RAND_MAX; /* 0.0 .. 1.0 */
    return (fraction * 2.0 - 1.0) * amount;
}

static int sim_connect(PrinterDriver *self) {
    printer_state_lock(self->state);
    self->state->connected = 1;
    printer_state_unlock(self->state);
    return 0;
}

static void sim_disconnect(PrinterDriver *self) {
    printer_state_lock(self->state);
    self->state->connected = 0;
    printer_state_unlock(self->state);
}

static int sim_jog(PrinterDriver *self, char axis, double delta_mm) {
    printer_state_lock(self->state);

    /* Deliberately no bounds-checking here (e.g. against bed size or max
     * Z) — that's the API layer's job (see api_handlers.c), which has
     * access to the printer profile and can clamp before ever calling
     * this. This driver's only responsibility is "move by this much",
     * same as how a real printer just obeys the G-code it's sent. */
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
        printer_state_unlock(self->state);
        return -1; /* unknown axis */
    }

    printer_state_unlock(self->state);
    return 0;
}

static int sim_home(PrinterDriver *self) {
    printer_state_lock(self->state);
    self->state->position.x_mm = 0.0;
    self->state->position.y_mm = 0.0;
    self->state->position.z_mm = 0.0;
    /* Homing doesn't reset the extruder position (e_mm) — that's how real
     * printers behave too, "home" is about X/Y/Z reference points. */
    printer_state_unlock(self->state);
    return 0;
}

static int sim_set_target_temp(PrinterDriver *self, PrinterHeater heater, double celsius) {
    printer_state_lock(self->state);
    if (heater == PRINTER_HEATER_HOTEND) {
        self->state->hotend.target_c = celsius;
    } else {
        self->state->bed.target_c = celsius;
    }
    printer_state_unlock(self->state);
    return 0;
}

/* Real temperature sensors can't read below 0 (well below room
 * temperature, let alone possible), so clamp jitter from ever pushing a
 * "resting at 0 target" reading slightly negative — a small cosmetic fix,
 * but negative temperatures in the API response would look like a bug to
 * anyone reading it. */
static double clamp_min_zero(double value) {
    return (value < 0.0) ? 0.0 : value;
}

static void sim_tick(PrinterDriver *self) {
    printer_state_lock(self->state);

    double hotend_next = step_toward(self->state->hotend.current_c, self->state->hotend.target_c,
                                     HOTEND_STEP_PER_TICK) +
                         jitter(0.05);
    self->state->hotend.current_c = clamp_min_zero(hotend_next);

    double bed_next =
        step_toward(self->state->bed.current_c, self->state->bed.target_c, BED_STEP_PER_TICK) +
        jitter(0.03);
    self->state->bed.current_c = clamp_min_zero(bed_next);

    printer_state_unlock(self->state);
}

PrinterDriver *transport_sim_create(PrinterState *state) {
    PrinterDriver *driver = malloc(sizeof(PrinterDriver));
    if (driver == NULL) {
        return NULL; /* out of memory — extremely unlikely for one small
                      * struct, but callers should still check for this */
    }

    driver->connect = sim_connect;
    driver->disconnect = sim_disconnect;
    driver->jog = sim_jog;
    driver->home = sim_home;
    driver->set_target_temp = sim_set_target_temp;
    driver->tick = sim_tick;
    driver->impl_data = NULL; /* the simulator has no private data to track */
    driver->state = state;

    return driver;
}

void transport_sim_destroy(PrinterDriver *driver) {
    free(driver);
}
