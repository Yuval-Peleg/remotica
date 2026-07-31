#ifndef REMOTICA_PRINTER_STATE_H
#define REMOTICA_PRINTER_STATE_H

/*
 * printer_state.h
 * ================
 * This file defines PrinterState: the single in-memory struct that holds
 * "what is currently true about the printer" — temperatures, position,
 * connection status, and print job progress.
 *
 * Why this exists (the "single source of truth" idea):
 * Lots of different parts of the backend need to read or change this data:
 *   - REST handlers (e.g. "GET /api/state" needs to read it,
 *     "POST /api/jog" needs to change the position)
 *   - The WebSocket broadcaster needs to read it every tick to push updates
 *     out to connected browsers
 *   - The printer driver (simulated or real serial) needs to update it as
 *     the printer's real-world state changes (temperatures drifting,
 *     position moving, etc.)
 * Rather than every piece of code keeping its own copy (which would drift
 * out of sync with reality), they all read/write this ONE shared struct.
 *
 * Why there's a mutex (pthread_mutex_t) inside it:
 * civetweb (our HTTP/WebSocket library) handles multiple requests at the
 * same time using a pool of worker threads. On top of that, we run a
 * separate background thread that "ticks" the printer driver forward
 * (see transport_sim.c / transport_serial.c). That means MULTIPLE THREADS
 * can try to read or write this struct at the same time. Without a mutex,
 * two threads writing at once could corrupt the data (a classic C bug
 * called a "data race"). The mutex makes sure only one thread touches the
 * struct at a time: you call printer_state_lock() before reading/writing
 * any field, and printer_state_unlock() when you're done.
 */

#include <pthread.h>

/* How far along the current print job is. Mirrors the frontend's derived
 * "Idle / Ready to print / Printing" states (see frontend/src/pages/
 * Dashboard.jsx) so the backend and frontend agree on what these mean. */
typedef enum {
    JOB_STATUS_IDLE,     /* no file selected */
    JOB_STATUS_READY,    /* a file is selected, but printing hasn't started */
    JOB_STATUS_PRINTING, /* actively printing (simulated progress for now) */
    JOB_STATUS_PAUSED    /* printing was started, then paused mid-job */
} JobStatus;

/* A single temperature reading: what it is right now, and what it's
 * supposed to reach. Used for both the hotend and the bed. */
typedef struct {
    double current_c;
    double target_c;
} TempReading;

/* Where the print head currently is, in millimeters from the origin.
 * e_mm is the cumulative amount of filament fed through the extruder —
 * it only ever goes up (or resets), it's not a "position" in the same
 * geometric sense as x/y/z. */
typedef struct {
    double x_mm;
    double y_mm;
    double z_mm;
    double e_mm;
} PrinterPosition;

/* The state of whatever gcode file is currently queued or printing. */
typedef struct {
    JobStatus status;
    char filename[256];      /* empty string ("") if no file is queued */
    double progress_percent; /* 0.0 - 100.0, only meaningful while PRINTING */
} PrintJob;

/* The full snapshot of "everything we know about the printer right now". */
typedef struct {
    pthread_mutex_t lock;

    int connected;            /* 1 if the driver has a live connection, else 0 */
    char connection_type[16]; /* "usb" or "wifi" — for now the driver always
                               * reports "usb", matching how a 3D printer is
                               * actually normally connected */

    TempReading hotend;
    TempReading bed;
    PrinterPosition position;

    PrintJob job;
} PrinterState;

/* Sets every field to a sensible starting value (disconnected, 0 temps,
 * origin position, no job) and initializes the mutex. Call this once,
 * before any other thread touches the struct. */
void printer_state_init(PrinterState *state);

/* Lock/unlock the state for exclusive access. Always pair these calls —
 * lock, do your reads/writes, unlock. Keep the locked section short (don't
 * do slow work like file I/O while holding the lock) so other threads
 * aren't kept waiting. */
void printer_state_lock(PrinterState *state);
void printer_state_unlock(PrinterState *state);

/* Builds a fresh cJSON object representing the current state, safe to call
 * from any thread — it locks internally, copies out the fields it needs,
 * and unlocks before doing the (slower) JSON-building work. The caller
 * owns the returned object and must cJSON_Delete() it when done. */
struct cJSON;
struct cJSON *printer_state_to_json(PrinterState *state);

#endif /* REMOTICA_PRINTER_STATE_H */
