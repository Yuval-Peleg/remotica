#ifndef REMOTICA_JOB_MANAGER_H
#define REMOTICA_JOB_MANAGER_H

/*
 * job_manager.h
 * ==============
 * Everything to do with "a gcode file the user wants to print": saving an
 * uploaded file to disk, starting/cancelling a print, and (since there's
 * no real printer streaming yet) faking progress advancing over time.
 *
 * This is deliberately kept separate from the printer driver
 * (transport.h) — the driver's job is "talk to the printer hardware",
 * this module's job is "manage the print job's lifecycle". A real print
 * would eventually need this module to read the queued gcode file line by
 * line and hand each line to the driver — see job_manager_tick()'s
 * comment for exactly what's NOT implemented yet.
 *
 * The job's current status/filename/progress live directly on the shared
 * PrinterState (see printer_state.h's PrintJob struct) rather than in a
 * separate struct here, since that's the same "single source of truth"
 * the rest of the backend already reads from.
 */

#include <stddef.h>

#include "printer_state.h"

/* Saves `body` (the raw bytes of an uploaded file, `body_len` bytes long)
 * to disk under `uploads_dir`, using `filename` as the file's name.
 *
 * `filename` is treated as untrusted input (it comes from the HTTP
 * request) — this function rejects anything that isn't a plain filename
 * (no "/", no "..", nothing that could escape uploads_dir and write
 * somewhere it shouldn't). See the .c file for details.
 *
 * On success, also updates `state`'s job to JOB_STATUS_READY with this
 * filename and 0 progress — matching what the frontend already does when
 * you pick a file (see GcodeDropzone.jsx), just now reflected on the
 * backend's shared state too.
 *
 * Returns 0 on success, -1 on failure (bad filename, or a disk error). */
int job_manager_save_upload(PrinterState *state, const char *uploads_dir, const char *filename,
                            const char *body, size_t body_len);

/* Marks the currently-queued file (state->job.filename) as printing.
 * Returns 0 on success, -1 if there's no file queued (state->job.status
 * isn't JOB_STATUS_READY) or the file doesn't actually exist on disk. */
int job_manager_start_print(PrinterState *state, const char *uploads_dir);

/* Stops printing and fully clears the queued job (status becomes
 * JOB_STATUS_IDLE, filename is cleared, progress resets to 0) — this
 * backend only tracks one job slot at a time, so "cancel" doubles as
 * "clear", matching there being no separate "remove file" endpoint yet. */
void job_manager_cancel_print(PrinterState *state);

/* Called on every tick (see main.c's background tick loop). If a print is
 * currently in progress, advances its progress by a small fixed amount
 * and marks it complete once it reaches 100%.
 *
 * *** This is a fake progress bar, not a real print. *** A real
 * implementation would read the queued gcode file and send it to the
 * printer driver (see transport.h) one line at a time, using actual
 * progress through the file (bytes sent / total bytes, or line number /
 * total lines) instead of a timer. That's a meaningfully bigger piece of
 * work — it needs to run on its own thread (so a multi-hour print doesn't
 * block the tick loop that also handles temperature polling and
 * WebSocket broadcasts), needs to handle the driver reporting errors
 * mid-print, and needs to support being paused/cancelled cleanly. This
 * function is a placeholder for that, so the REST API and frontend have
 * something real to talk to today. */
void job_manager_tick(PrinterState *state);

#endif /* REMOTICA_JOB_MANAGER_H */
