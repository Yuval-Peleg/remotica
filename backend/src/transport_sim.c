/*
 * transport_sim.c
 * ================
 * See transport_sim.h for the overview. This file fills in the
 * PrinterDriver function pointers with "pretend" behavior.
 */

#include "transport_sim.h"

#include <stdio.h>
#include <stdlib.h>

#include "console_log.h"

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

/* Records a synthesized command as "sent", then immediately a synthesized
 * "ok" as "received" — the simulator answers instantly, so there's no
 * real round-trip to represent, but logging both sides keeps the
 * console's shape consistent with what the real serial driver logs (see
 * send_and_wait_for_ok in transport_serial.c). Does nothing if this
 * driver wasn't given a console to log to (see transport_sim_create). */
static void log_command(PrinterDriver *self, const char *line) {
    if (self->console == NULL) {
        return;
    }
    console_log_append(self->console, CONSOLE_DIRECTION_SENT, line);
    console_log_append(self->console, CONSOLE_DIRECTION_RECEIVED, "ok");
}

static int sim_jog(PrinterDriver *self, char axis, double delta_mm) {
    if (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'E') {
        return -1; /* unknown axis */
    }

    /* Synthesize the same three-line relative-move sequence
     * transport_serial.c's serial_jog() actually sends to a real printer
     * — see JOG_FEED_RATE_* in transport.h for where the feed rates come
     * from (shared with the real driver so this matches exactly). */
    double feed_rate = JOG_FEED_RATE_XY;
    if (axis == 'Z') {
        feed_rate = JOG_FEED_RATE_Z;
    } else if (axis == 'E') {
        feed_rate = JOG_FEED_RATE_E;
    }

    char move_command[128];
    snprintf(move_command, sizeof(move_command), "G1 %c%.3f F%.0f", axis, delta_mm, feed_rate);

    log_command(self, "G91");
    log_command(self, move_command);
    log_command(self, "G90");

    /* Deliberately no bounds-checking here (e.g. against bed size or max
     * Z) — that's the API layer's job (see api_handlers.c), which has
     * access to the printer profile and can clamp before ever calling
     * this. This driver's only responsibility is "move by this much",
     * same as how a real printer just obeys the G-code it's sent. */
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
    }
    printer_state_unlock(self->state);

    return 0;
}

static int sim_home(PrinterDriver *self) {
    log_command(self, "G28");

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
    char command[32];
    if (heater == PRINTER_HEATER_HOTEND) {
        snprintf(command, sizeof(command), "M104 S%.1f", celsius);
    } else {
        snprintf(command, sizeof(command), "M140 S%.1f", celsius);
    }
    log_command(self, command);

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

PrinterDriver *transport_sim_create(PrinterState *state, ConsoleLog *console) {
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
    driver->console = console;

    return driver;
}

void transport_sim_destroy(PrinterDriver *driver) {
    free(driver);
}
