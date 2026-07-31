/*
 * job_manager.c
 * ==============
 * See job_manager.h for the overview. This file is the "how": validating
 * filenames, writing uploaded bytes to disk, and the fake progress timer.
 */

#include "job_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* How much fake "progress" to add per tick while printing. main.c ticks
 * roughly every 300ms, so 100 ticks (100 / (100/30) ≈ 30 seconds) to go
 * from 0% to 100% — fast enough to actually watch a demo print finish
 * without waiting minutes for it. */
#define PROGRESS_PERCENT_PER_TICK (100.0 / 100.0)

/* Checks that `filename` is safe to use as-is inside a file path we
 * build ourselves (uploads_dir + "/" + filename). Since filename comes
 * straight from an HTTP request, a malicious or just-buggy client could
 * send something like "../../etc/passwd" or "/etc/passwd" and, without
 * this check, we'd happily write to wherever that points — a classic
 * "path traversal" bug. This rejects anything containing a '/' (so it
 * can't escape the uploads directory or be an absolute path) or the
 * two-character sequence ".." (so it can't walk back up a directory
 * tree even without a literal slash in some filesystem edge cases), plus
 * empty names. */
static int is_safe_filename(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return 0;
    }
    if (strstr(filename, "..") != NULL) {
        return 0;
    }
    if (strchr(filename, '/') != NULL) {
        return 0;
    }
    return 1;
}

/* Creates `dir` if it doesn't already exist. Ignores the "already exists"
 * case (that's the normal, expected situation after the first run). */
static void ensure_directory_exists(const char *dir) {
    mkdir(dir, 0755);
}

static void build_upload_path(char *out, size_t out_size, const char *uploads_dir,
                              const char *filename) {
    snprintf(out, out_size, "%s/%s", uploads_dir, filename);
}

int job_manager_save_upload(PrinterState *state, const char *uploads_dir, const char *filename,
                            const char *body, size_t body_len) {
    if (!is_safe_filename(filename)) {
        return -1;
    }

    ensure_directory_exists(uploads_dir);

    char path[512];
    build_upload_path(path, sizeof(path), uploads_dir, filename);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }

    size_t written = fwrite(body, 1, body_len, file);
    fclose(file);

    if (written != body_len) {
        return -1; /* disk full, or some other write failure */
    }

    printer_state_lock(state);
    strncpy(state->job.filename, filename, sizeof(state->job.filename) - 1);
    state->job.filename[sizeof(state->job.filename) - 1] = '\0';
    state->job.status = JOB_STATUS_READY;
    state->job.progress_percent = 0.0;
    printer_state_unlock(state);

    return 0;
}

int job_manager_start_print(PrinterState *state, const char *uploads_dir) {
    printer_state_lock(state);

    if (state->job.status != JOB_STATUS_READY) {
        printer_state_unlock(state);
        return -1; /* nothing queued, or already printing */
    }

    char path[512];
    build_upload_path(path, sizeof(path), uploads_dir, state->job.filename);

    printer_state_unlock(state);

    /* Confirm the file is actually still there before committing to
     * "printing" it — it could in principle have been removed from disk
     * since it was uploaded. stat() outside the lock is fine: we're not
     * touching shared state here, just checking the filesystem. */
    struct stat file_info;
    if (stat(path, &file_info) != 0) {
        return -1;
    }

    printer_state_lock(state);
    state->job.status = JOB_STATUS_PRINTING;
    state->job.progress_percent = 0.0;
    printer_state_unlock(state);

    return 0;
}

void job_manager_cancel_print(PrinterState *state) {
    printer_state_lock(state);
    state->job.status = JOB_STATUS_IDLE;
    state->job.filename[0] = '\0';
    state->job.progress_percent = 0.0;
    printer_state_unlock(state);
}

void job_manager_tick(PrinterState *state) {
    printer_state_lock(state);

    if (state->job.status == JOB_STATUS_PRINTING) {
        state->job.progress_percent += PROGRESS_PERCENT_PER_TICK;
        if (state->job.progress_percent >= 100.0) {
            state->job.progress_percent = 100.0;
            /* "Finished" printing: go back to READY rather than IDLE, so
             * the same file is still queued and could be printed again
             * without re-uploading it. */
            state->job.status = JOB_STATUS_READY;
        }
    }

    printer_state_unlock(state);
}
