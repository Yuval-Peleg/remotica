/*
 * transport_sim.c
 * ================
 * See transport_sim.h for the overview. This file fills in the
 * PrinterDriver function pointers with "pretend" behavior.
 */

/* -std=c99 hides usleep() unless we ask glibc for its normal feature set
 * first — same reason job_manager.c and main.c both do this, must come
 * before any system header is included. */
#define _DEFAULT_SOURCE

#include "transport_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "console_log.h"

/* This driver's private data. Only used to track two bits of "modal
 * state" from G90/G91 (absolute vs. relative positioning) and M82/M83
 * (absolute vs. relative extrusion) — real gcode files toggle these, and
 * getting them right is needed to make sim_send_gcode_line's position
 * tracking (see below) actually match what the line means. Both default
 * to absolute, same as real Marlin firmware's defaults. */
typedef struct {
    int relative_position;  /* 0 = absolute (G90), 1 = relative (G91) */
    int relative_extrusion; /* 0 = absolute (M82), 1 = relative (M83) */
} SimImplData;

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
    /* Mirrors what a real M115 reply would populate on transport_serial.c
     * (see query_firmware_info there), so the Settings page's "detected
     * firmware" hint has something to show in the default sim-driven demo
     * too, not just when real hardware is attached. */
    strncpy(self->state->firmware_info,
            "FIRMWARE_NAME:Remotica Simulator MACHINE_TYPE:Simulated Printer",
            sizeof(self->state->firmware_info) - 1);
    self->state->firmware_info[sizeof(self->state->firmware_info) - 1] = '\0';
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

/* Looks for `letter` immediately followed by a signed number anywhere in
 * `line` (e.g. looking for 'X' in "G1 X12.5 Y3 F1200" finds "X12.5" and
 * writes 12.5 to *out). Keeps searching past false matches — a letter
 * with no number right after it doesn't count — so this can't be tripped
 * up by, say, an 'F' inside "F1200" being mistaken for a different
 * parameter. Returns 1 if found, 0 otherwise. */
static int parse_gcode_value(const char *line, char letter, double *out) {
    const char *p = strchr(line, letter);
    while (p != NULL) {
        char *end;
        double value = strtod(p + 1, &end);
        if (end != p + 1) {
            *out = value;
            return 1;
        }
        p = strchr(p + 1, letter);
    }
    return 0;
}

/* Interprets just enough of a real gcode line to keep the simulated
 * position/temperatures moving realistically during a print — this is
 * NOT a full gcode parser (no arcs, no bed leveling, no multi-extruder
 * commands), just the handful of commands job_manager.c's print streamer
 * actually needs simulated feedback for. The real serial driver doesn't
 * need any of this: it just relays lines to actual hardware, which
 * interprets them itself. */
/* A real printer isn't instant: transmitting a line over serial and
 * waiting for the firmware's "ok" takes real time (see transport_serial.c),
 * and a print is thousands of such round-trips. Without some per-line
 * delay here, a simulated print streams end-to-end in well under a
 * second — too fast to actually watch progress/time-left/the temp graph
 * do anything. This runs on job_manager.c's dedicated streamer thread
 * (see streamer_thread_main), never the shared tick thread, so sleeping
 * here doesn't stall temperature polling or WebSocket broadcasts. */
#define SIM_LINE_DELAY_US 15000

static int sim_send_gcode_line(PrinterDriver *self, const char *line) {
    SimImplData *impl = (SimImplData *)self->impl_data;
    log_command(self, line);
    usleep(SIM_LINE_DELAY_US);

    if (strncmp(line, "G90", 3) == 0) {
        impl->relative_position = 0;
    } else if (strncmp(line, "G91", 3) == 0) {
        impl->relative_position = 1;
    } else if (strncmp(line, "M82", 3) == 0) {
        impl->relative_extrusion = 0;
    } else if (strncmp(line, "M83", 3) == 0) {
        impl->relative_extrusion = 1;
    } else if (strncmp(line, "G28", 3) == 0) {
        printer_state_lock(self->state);
        self->state->position.x_mm = 0.0;
        self->state->position.y_mm = 0.0;
        self->state->position.z_mm = 0.0;
        printer_state_unlock(self->state);
    } else if (strncmp(line, "G0", 2) == 0 || strncmp(line, "G1", 2) == 0) {
        double x, y, z, e;
        int has_x = parse_gcode_value(line, 'X', &x);
        int has_y = parse_gcode_value(line, 'Y', &y);
        int has_z = parse_gcode_value(line, 'Z', &z);
        int has_e = parse_gcode_value(line, 'E', &e);

        printer_state_lock(self->state);
        if (has_x) {
            self->state->position.x_mm =
                impl->relative_position ? self->state->position.x_mm + x : x;
        }
        if (has_y) {
            self->state->position.y_mm =
                impl->relative_position ? self->state->position.y_mm + y : y;
        }
        if (has_z) {
            self->state->position.z_mm =
                impl->relative_position ? self->state->position.z_mm + z : z;
        }
        if (has_e) {
            self->state->position.e_mm =
                impl->relative_extrusion ? self->state->position.e_mm + e : e;
        }
        printer_state_unlock(self->state);
    } else if (strncmp(line, "M104", 4) == 0 || strncmp(line, "M109", 4) == 0) {
        double s;
        if (parse_gcode_value(line, 'S', &s)) {
            printer_state_lock(self->state);
            self->state->hotend.target_c = s;
            printer_state_unlock(self->state);
        }
    } else if (strncmp(line, "M140", 4) == 0 || strncmp(line, "M190", 4) == 0) {
        double s;
        if (parse_gcode_value(line, 'S', &s)) {
            printer_state_lock(self->state);
            self->state->bed.target_c = s;
            printer_state_unlock(self->state);
        }
    }
    /* Anything else (fan speed, retraction settings, etc.) is logged to
     * the console above but otherwise ignored — it doesn't affect
     * anything this simulator tracks. */

    return 0;
}

PrinterDriver *transport_sim_create(PrinterState *state, ConsoleLog *console) {
    PrinterDriver *driver = malloc(sizeof(PrinterDriver));
    if (driver == NULL) {
        return NULL; /* out of memory — extremely unlikely for one small
                      * struct, but callers should still check for this */
    }

    SimImplData *impl = malloc(sizeof(SimImplData));
    if (impl == NULL) {
        free(driver);
        return NULL;
    }
    impl->relative_position = 0;
    impl->relative_extrusion = 0;

    driver->connect = sim_connect;
    driver->disconnect = sim_disconnect;
    driver->jog = sim_jog;
    driver->home = sim_home;
    driver->set_target_temp = sim_set_target_temp;
    driver->tick = sim_tick;
    driver->send_gcode_line = sim_send_gcode_line;
    driver->impl_data = impl;
    driver->state = state;
    driver->console = console;

    return driver;
}

void transport_sim_destroy(PrinterDriver *driver) {
    free(driver->impl_data);
    free(driver);
}
