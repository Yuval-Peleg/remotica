/*
 * job_manager.c
 * ==============
 * See job_manager.h for the overview. This file is the "how": validating
 * filenames, writing uploaded bytes to disk, and the fake progress timer.
 */

#include "job_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Creates `dir` if it doesn't already exist — including any missing
 * parent directories, like the shell's `mkdir -p` (plain mkdir() only
 * creates ONE level and fails if its parent is also missing, which
 * matters here since uploads_dir is "data/uploads": on a fresh checkout
 * neither "data" nor "data/uploads" exist yet, so we need to create
 * "data" first before "data/uploads" can succeed).
 *
 * Works by walking the path left to right, temporarily truncating the
 * string at each '/' to mkdir() just that prefix, then restoring the '/'
 * and moving on — so for "data/uploads" it creates "data", then
 * "data/uploads". Failures (including "already exists") are ignored at
 * each step on purpose: the only thing that matters is whether the full
 * directory exists by the time this returns, not how many of the steps
 * were actually necessary. */
static void ensure_directory_exists(const char *dir) {
    char path[512];
    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    for (char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(path, 0755);
        *slash = '/';
    }
    mkdir(path, 0755);
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

int job_manager_pause_print(PrinterState *state) {
    printer_state_lock(state);

    int was_printing = (state->job.status == JOB_STATUS_PRINTING);
    if (was_printing) {
        state->job.status = JOB_STATUS_PAUSED;
    }

    printer_state_unlock(state);
    return was_printing ? 0 : -1;
}

int job_manager_resume_print(PrinterState *state) {
    printer_state_lock(state);

    int was_paused = (state->job.status == JOB_STATUS_PAUSED);
    if (was_paused) {
        state->job.status = JOB_STATUS_PRINTING;
    }

    printer_state_unlock(state);
    return was_paused ? 0 : -1;
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
    /* JOB_STATUS_PAUSED is deliberately left alone here — progress simply
     * doesn't move forward while paused, which is the whole point of
     * pausing. */

    printer_state_unlock(state);
}

/* ---------------------------------------------------------------------
 * "On device" file management
 * --------------------------------------------------------------------- */

/* True if `filename` ends with ".gcode" (case-sensitive, matching the
 * same extension check the frontend's useGcodeFile hook already does). */
static int has_gcode_extension(const char *filename) {
    size_t len = strlen(filename);
    const char *suffix = ".gcode";
    size_t suffix_len = strlen(suffix);
    if (len < suffix_len) {
        return 0;
    }
    return strcmp(filename + (len - suffix_len), suffix) == 0;
}

int job_manager_list_files(const char *uploads_dir, GcodeFileInfo *out_files, int max_files) {
    DIR *dir = opendir(uploads_dir);
    if (dir == NULL) {
        /* Most likely reason: nothing has ever been uploaded yet, so the
         * directory was never created (see ensure_directory_exists in
         * job_manager_save_upload). That's zero files, not an error. */
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while (count < max_files && (entry = readdir(dir)) != NULL) {
        if (!has_gcode_extension(entry->d_name)) {
            continue; /* skip ".", "..", and anything that isn't a .gcode file */
        }

        char path[512];
        build_upload_path(path, sizeof(path), uploads_dir, entry->d_name);

        struct stat file_info;
        if (stat(path, &file_info) != 0) {
            continue; /* file vanished between readdir() and stat(), skip it */
        }

        GcodeFileInfo *out = &out_files[count];
        strncpy(out->filename, entry->d_name, sizeof(out->filename) - 1);
        out->filename[sizeof(out->filename) - 1] = '\0';
        out->size_bytes = (long long)file_info.st_size;
        out->modified_unix_time = (long long)file_info.st_mtime;

        count++;
    }

    closedir(dir);
    return count;
}

int job_manager_file_exists(const char *uploads_dir, const char *filename) {
    if (!is_safe_filename(filename)) {
        return 0;
    }

    char path[512];
    build_upload_path(path, sizeof(path), uploads_dir, filename);

    struct stat file_info;
    return (stat(path, &file_info) == 0) ? 1 : 0;
}

int job_manager_select_existing(PrinterState *state, const char *uploads_dir,
                                const char *filename) {
    if (!job_manager_file_exists(uploads_dir, filename)) {
        return -1;
    }

    printer_state_lock(state);
    strncpy(state->job.filename, filename, sizeof(state->job.filename) - 1);
    state->job.filename[sizeof(state->job.filename) - 1] = '\0';
    state->job.status = JOB_STATUS_READY;
    state->job.progress_percent = 0.0;
    printer_state_unlock(state);

    return 0;
}

int job_manager_delete_file(PrinterState *state, const char *uploads_dir, const char *filename) {
    if (!is_safe_filename(filename)) {
        return -1;
    }

    printer_state_lock(state);
    int is_active_job =
        (strcmp(state->job.filename, filename) == 0) &&
        (state->job.status == JOB_STATUS_PRINTING || state->job.status == JOB_STATUS_PAUSED);
    printer_state_unlock(state);

    if (is_active_job) {
        return -1; /* refuse to delete a file that's actively printing */
    }

    char path[512];
    build_upload_path(path, sizeof(path), uploads_dir, filename);

    if (unlink(path) != 0) {
        return -1;
    }

    /* If the file we just deleted was the queued-but-not-printing job,
     * there's nothing left to print — clear the selection so the
     * frontend doesn't show a "ready to print" file that no longer
     * exists. */
    printer_state_lock(state);
    if (strcmp(state->job.filename, filename) == 0) {
        state->job.status = JOB_STATUS_IDLE;
        state->job.filename[0] = '\0';
        state->job.progress_percent = 0.0;
    }
    printer_state_unlock(state);

    return 0;
}
