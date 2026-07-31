/*
 * job_manager.c
 * ==============
 * See job_manager.h for the overview. This file is the "how": validating
 * filenames, writing uploaded bytes to disk, and the real gcode print
 * streamer (a dedicated background thread per print).
 */

/* -std=c99 hides usleep() unless we ask glibc for its normal feature set
 * first — same reason main.c and transport_serial.c both do this, must
 * come before any system header is included. */
#define _DEFAULT_SOURCE

#include "job_manager.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How often the streamer thread re-checks for a pause/cancel while
 * sitting idle in JOB_STATUS_PAUSED, in milliseconds — short enough that
 * resuming/cancelling a paused print feels immediate, long enough not to
 * busy-loop. */
#define PAUSE_POLL_INTERVAL_MS 200

/* ---------------------------------------------------------------------
 * Print streamer — a dedicated background thread per print (see
 * job_manager_start_print). Only one can be active at a time (enforced by
 * the job status state machine: start_print requires JOB_STATUS_READY,
 * which can't be true again until the previous print has finished or
 * been cancelled), so a single set of static fields is enough to track
 * it — no need for a heap-allocated "job manager instance" the rest of
 * the backend would have to thread through everywhere.
 * --------------------------------------------------------------------- */
static struct {
    pthread_mutex_t mutex; /* guards every field below */
    pthread_t thread;
    int thread_valid;     /* 1 if `thread` refers to a not-yet-joined thread */
    int cancel_requested; /* set by job_manager_cancel_print/_shutdown */
} s_streamer = {.mutex = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
    PrinterState *state;
    PrinterDriver *driver;
    char path[512];
} StreamerArgs;

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

/* Strips a trailing "; comment" (if any) and surrounding whitespace from
 * `raw_line` in place, then returns a pointer to the first real
 * character — or NULL if nothing's left (a blank line, or a line that
 * was only ever a comment). Lines that reduce to NULL are never sent to
 * the printer: real gcode senders don't forward blank/comment lines
 * either, and Marlin would just waste a round-trip acknowledging one.
 *
 * `raw_line` is expected to be at most GCODE_LINE_BUF_SIZE bytes
 * (including the trailing '\n' fgets() leaves in place) — real sliced
 * gcode lines are virtually always well under 100 bytes, so a line
 * somehow longer than that buffer gets split across two "lines" here
 * instead of one, a known simplification rather than a full streaming
 * line reader. */
#define GCODE_LINE_BUF_SIZE 1024

static char *strip_gcode_line(char *raw_line) {
    char *semicolon = strchr(raw_line, ';');
    if (semicolon != NULL) {
        *semicolon = '\0';
    }

    size_t len = strlen(raw_line);
    while (len > 0 && (raw_line[len - 1] == '\n' || raw_line[len - 1] == '\r' ||
                       raw_line[len - 1] == ' ' || raw_line[len - 1] == '\t')) {
        raw_line[--len] = '\0';
    }

    char *start = raw_line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    return (*start == '\0') ? NULL : start;
}

/* Counts how many lines in `file` are actually real gcode (i.e. would
 * survive strip_gcode_line) — used up front to compute a percentage as
 * the streamer works through the file. Leaves the file position at EOF;
 * callers rewind() before actually streaming. */
static long count_printable_lines(FILE *file) {
    long count = 0;
    char buf[GCODE_LINE_BUF_SIZE];
    while (fgets(buf, sizeof(buf), file) != NULL) {
        if (strip_gcode_line(buf) != NULL) {
            count++;
        }
    }
    return count;
}

/* The body of the print-streaming background thread launched by
 * job_manager_start_print(). Reads `args->path` line by line and hands
 * each real gcode line to the driver, tracking progress on `args->state`
 * as it goes. Frees `args` and returns when the file is exhausted, the
 * driver rejects a line, or a pause/cancel changes the job out from
 * under it. */
static void *streamer_thread_main(void *arg) {
    StreamerArgs *args = (StreamerArgs *)arg;
    PrinterState *state = args->state;
    PrinterDriver *driver = args->driver;

    FILE *file = fopen(args->path, "r");
    if (file == NULL) {
        printer_state_lock(state);
        state->job.status = JOB_STATUS_READY; /* couldn't open it — don't get stuck "printing" */
        printer_state_unlock(state);
        free(args);
        return NULL;
    }

    long total_lines = count_printable_lines(file);
    rewind(file);
    if (total_lines <= 0) {
        total_lines = 1; /* avoid a divide-by-zero below for an empty file */
    }

    long sent_lines = 0;
    int aborted = 0;
    char raw_line[GCODE_LINE_BUF_SIZE];

    while (fgets(raw_line, sizeof(raw_line), file) != NULL) {
        char *line = strip_gcode_line(raw_line);
        if (line == NULL) {
            continue; /* blank or comment-only — never sent to the printer */
        }

        /* While paused, sit here polling instead of sending anything —
         * exits as soon as either the job leaves PAUSED (resumed, or
         * cancelled out from under the pause) or a cancel is requested
         * directly. */
        for (;;) {
            printer_state_lock(state);
            JobStatus status = state->job.status;
            printer_state_unlock(state);

            pthread_mutex_lock(&s_streamer.mutex);
            int cancel = s_streamer.cancel_requested;
            pthread_mutex_unlock(&s_streamer.mutex);

            if (cancel || status != JOB_STATUS_PAUSED) {
                break;
            }
            usleep(PAUSE_POLL_INTERVAL_MS * 1000);
        }

        pthread_mutex_lock(&s_streamer.mutex);
        int cancel = s_streamer.cancel_requested;
        pthread_mutex_unlock(&s_streamer.mutex);
        if (cancel) {
            aborted = 1;
            break;
        }

        printer_state_lock(state);
        JobStatus status = state->job.status;
        printer_state_unlock(state);
        if (status != JOB_STATUS_PRINTING) {
            /* Job was cancelled (or otherwise moved on) through some
             * other path while we were between lines. */
            aborted = 1;
            break;
        }

        if (driver->send_gcode_line(driver, line) != 0) {
            /* The printer didn't acknowledge this line. Treat that as
             * fatal for this print rather than silently skipping ahead —
             * silently dropping a motion or temperature command could
             * leave a real printer in a genuinely bad physical state
             * (head crashed into the bed, heater left on, ...). */
            aborted = 1;
            break;
        }

        sent_lines++;
        printer_state_lock(state);
        state->job.progress_percent = ((double)sent_lines / (double)total_lines) * 100.0;
        printer_state_unlock(state);
    }

    fclose(file);

    printer_state_lock(state);
    if (!aborted) {
        state->job.progress_percent = 100.0;
        /* "Finished" printing: go back to READY rather than IDLE, so the
         * same file is still queued and could be printed again without
         * re-uploading it. */
        state->job.status = JOB_STATUS_READY;
    }
    /* If aborted, leave state->job exactly as whatever cancelled it left
     * it (job_manager_cancel_print already set JOB_STATUS_IDLE) — nothing
     * to do here. */
    printer_state_unlock(state);

    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.cancel_requested = 0;
    pthread_mutex_unlock(&s_streamer.mutex);

    free(args);
    return NULL;
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

int job_manager_start_print(PrinterState *state, PrinterDriver *driver, const char *uploads_dir) {
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

    StreamerArgs *args = malloc(sizeof(StreamerArgs));
    if (args == NULL) {
        return -1;
    }
    args->state = state;
    args->driver = driver;
    strncpy(args->path, path, sizeof(args->path) - 1);
    args->path[sizeof(args->path) - 1] = '\0';

    pthread_mutex_lock(&s_streamer.mutex);
    if (s_streamer.thread_valid) {
        /* The only way we get here with thread_valid still set is that
         * an earlier print's thread already ran to completion (job
         * status can't be READY again until it has) — this join reclaims
         * its resources and returns immediately rather than blocking. */
        pthread_join(s_streamer.thread, NULL);
        s_streamer.thread_valid = 0;
    }
    s_streamer.cancel_requested = 0;
    pthread_mutex_unlock(&s_streamer.mutex);

    /* Flip the job to PRINTING *before* the thread starts, not after —
     * streamer_thread_main checks state->job.status as soon as it starts
     * running, so if this happened afterward the thread could see the
     * still-stale JOB_STATUS_READY and immediately treat that as "someone
     * else cancelled me" and abort. */
    printer_state_lock(state);
    state->job.status = JOB_STATUS_PRINTING;
    state->job.progress_percent = 0.0;
    printer_state_unlock(state);

    pthread_mutex_lock(&s_streamer.mutex);
    int create_result = pthread_create(&s_streamer.thread, NULL, streamer_thread_main, args);
    s_streamer.thread_valid = (create_result == 0);
    pthread_mutex_unlock(&s_streamer.mutex);

    if (create_result != 0) {
        printer_state_lock(state);
        state->job.status = JOB_STATUS_READY;
        state->job.progress_percent = 0.0;
        printer_state_unlock(state);
        free(args);
        return -1;
    }

    return 0;
}

void job_manager_cancel_print(PrinterState *state) {
    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.cancel_requested = 1;
    pthread_mutex_unlock(&s_streamer.mutex);

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

void job_manager_shutdown(void) {
    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.cancel_requested = 1;
    int has_thread = s_streamer.thread_valid;
    pthread_t thread = s_streamer.thread;
    pthread_mutex_unlock(&s_streamer.mutex);

    if (!has_thread) {
        return;
    }

    pthread_join(thread, NULL);

    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.thread_valid = 0;
    pthread_mutex_unlock(&s_streamer.mutex);
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
