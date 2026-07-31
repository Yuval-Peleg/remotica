#ifndef REMOTICA_JOB_MANAGER_H
#define REMOTICA_JOB_MANAGER_H

/*
 * job_manager.h
 * ==============
 * Everything to do with "a gcode file the user wants to print": saving an
 * uploaded file to disk, and starting/pausing/resuming/cancelling a real
 * print.
 *
 * This is deliberately kept separate from the printer driver
 * (transport.h) — the driver's job is "talk to the printer hardware",
 * this module's job is "manage the print job's lifecycle". Once a print
 * starts, job_manager_start_print() launches a dedicated background
 * thread (see job_manager.c's streamer_thread_main) that reads the queued
 * gcode file line by line and hands each line to the driver's
 * send_gcode_line(), tracking real progress as it goes — this runs on its
 * own thread rather than the shared tick loop specifically so a
 * multi-hour print doesn't block temperature polling or WebSocket
 * broadcasts (see main.c).
 *
 * The job's current status/filename/progress live directly on the shared
 * PrinterState (see printer_state.h's PrintJob struct) rather than in a
 * separate struct here, since that's the same "single source of truth"
 * the rest of the backend already reads from.
 */

#include <stddef.h>

#include "printer_state.h"
#include "transport.h"

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

/* Marks the currently-queued file (state->job.filename) as printing and
 * launches the background thread that actually streams it to `driver`
 * line by line. Returns 0 on success, -1 if there's no file queued
 * (state->job.status isn't JOB_STATUS_READY), the file doesn't actually
 * exist on disk, or the thread couldn't be created. */
int job_manager_start_print(PrinterState *state, PrinterDriver *driver, const char *uploads_dir);

/* Stops printing and fully clears the queued job (status becomes
 * JOB_STATUS_IDLE, filename is cleared, progress resets to 0) — this
 * backend only tracks one job slot at a time, so "cancel" doubles as
 * "clear", matching there being no separate "remove file" endpoint yet.
 * If a print streaming thread is currently running, it notices the
 * cancellation and stops on its own (see streamer_thread_main) — this
 * function does not block waiting for that to happen. */
void job_manager_cancel_print(PrinterState *state);

/* Pauses an in-progress print (JOB_STATUS_PRINTING -> JOB_STATUS_PAUSED),
 * keeping the current progress percentage. Returns 0 on success, -1 if
 * not currently printing. */
int job_manager_pause_print(PrinterState *state);

/* Resumes a paused print (JOB_STATUS_PAUSED -> JOB_STATUS_PRINTING) from
 * wherever its progress was left off. Returns 0 on success, -1 if not
 * currently paused. */
int job_manager_resume_print(PrinterState *state);

/* Call once during shutdown (see main.c), before disconnecting the
 * printer driver. If a print streamer thread is currently running, this
 * asks it to stop and blocks until it actually has — without this, the
 * driver could be disconnected (e.g. its serial port closed) while the
 * streamer thread is still mid-way through sending it a line, which
 * would be a use-after-close bug. Safe to call even if no print is
 * running. */
void job_manager_shutdown(void);

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
