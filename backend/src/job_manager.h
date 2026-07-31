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

/* Pauses an in-progress print (JOB_STATUS_PRINTING -> JOB_STATUS_PAUSED),
 * keeping the current progress percentage. Returns 0 on success, -1 if
 * not currently printing. */
int job_manager_pause_print(PrinterState *state);

/* Resumes a paused print (JOB_STATUS_PAUSED -> JOB_STATUS_PRINTING) from
 * wherever its progress was left off. Returns 0 on success, -1 if not
 * currently paused. */
int job_manager_resume_print(PrinterState *state);

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

/* ---------------------------------------------------------------------
 * "On device" file management — every gcode file that's ever been
 * uploaded stays in uploads_dir until explicitly deleted, so it can be
 * re-printed later without uploading it again. These functions let the
 * REST API list, re-select, and delete those files.
 * --------------------------------------------------------------------- */

#define GCODE_FILENAME_MAX 256

typedef struct {
    char filename[GCODE_FILENAME_MAX];
    long long size_bytes;
    long long modified_unix_time; /* seconds since epoch, for sorting by
                                   * most-recently-uploaded in the UI */
} GcodeFileInfo;

/* Lists every .gcode file in uploads_dir (newest-uploaded first isn't
 * guaranteed by this function — the caller can sort by
 * modified_unix_time if that ordering matters), writing up to
 * max_files entries into out_files. Returns how many files were written,
 * or -1 if uploads_dir couldn't even be opened for some reason other
 * than "it doesn't exist yet" (a missing directory just means zero files
 * have ever been uploaded, and returns 0, not -1). */
int job_manager_list_files(const char *uploads_dir, GcodeFileInfo *out_files, int max_files);

/* Checks whether `filename` exists in uploads_dir and is a safe filename
 * — does NOT touch `state` at all. Used when the caller just wants to
 * confirm a file is really there (e.g. before serving its raw content
 * for a preview) without it having any side effect on what's currently
 * queued to print. Returns 1 if it exists and is safe, 0 otherwise. */
int job_manager_file_exists(const char *uploads_dir, const char *filename);

/* Marks an already-uploaded file as the queued job (JOB_STATUS_READY),
 * without re-uploading it — used by the "on device" file browser's
 * "print this again" action. Returns 0 on success, -1 if the file
 * doesn't exist on disk or the filename is unsafe. */
int job_manager_select_existing(PrinterState *state, const char *uploads_dir, const char *filename);

/* Deletes a file from uploads_dir. Refuses (returns -1) if that file is
 * the one currently printing or paused — deleting a file mid-print would
 * leave the job pointing at nothing. If the deleted file happens to be
 * the currently-*selected*-but-not-printing job, the job is reset to
 * JOB_STATUS_IDLE, since it no longer makes sense to "print" a file that
 * was just deleted. */
int job_manager_delete_file(PrinterState *state, const char *uploads_dir, const char *filename);

#endif /* REMOTICA_JOB_MANAGER_H */
