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

/* Why a print stopped early, if it did. */
typedef enum {
    ABORT_NONE = 0,      /* the file was streamed to the end */
    ABORT_CANCELLED,     /* a cancel/shutdown request, or the job status changed under us */
    ABORT_DRIVER_FAILURE /* the printer stopped acknowledging lines */
} AbortReason;

/* Leaves the printer in a safe state after a print stops early.
 *
 * This matters more than it might look: without it, cancelling a print
 * (or losing the printer mid-print) stops sending file lines and does
 * nothing else — hotend and bed stay at full print temperature
 * indefinitely, the part cooling fan keeps running, and the nozzle sits
 * parked against the (still molten) top surface of the part, slowly
 * melting a crater into it. Heaters off first, because an unattended
 * heater is the only part of that list that's an actual hazard rather
 * than a ruined print.
 *
 * Deliberately called from the streamer thread itself, never from
 * job_manager_cancel_print()'s HTTP-handler thread: the driver expects
 * one conversation at a time, and the streamer may still be blocked
 * waiting for the previous line's acknowledgement when the cancel
 * arrives.
 *
 * Every line is best-effort — if the printer has already stopped
 * answering there's nothing to be done about it, and we specifically
 * must not spend an ack timeout per line discovering that seven times
 * over, since job_manager_shutdown() blocks on this thread finishing
 * and that delay would land directly on Ctrl+C. So if the printer
 * doesn't acknowledge the heater-off commands, the rest is skipped. */
static void send_abort_safety_sequence(PrinterDriver *driver) {
    static const char *const heaters_off[] = {
        "M104 S0", /* hotend off */
        "M140 S0", /* bed off */
        "M107",    /* part cooling fan off */
    };
    static const char *const park_and_release[] = {
        "G91",        /* relative positioning, just for the lift below */
        "G1 Z5 F600", /* lift 5mm so the nozzle isn't resting on the part */
        "G90",        /* back to absolute — never leave modal state changed */
        "M84",        /* disable steppers (also lets the bed be moved by hand) */
    };

    int printer_responding = 1;
    for (size_t i = 0; i < sizeof(heaters_off) / sizeof(heaters_off[0]); i++) {
        if (driver->send_gcode_line(driver, heaters_off[i]) != 0) {
            printer_responding = 0;
        }
    }

    if (!printer_responding) {
        return;
    }

    for (size_t i = 0; i < sizeof(park_and_release) / sizeof(park_and_release[0]); i++) {
        if (driver->send_gcode_line(driver, park_and_release[i]) != 0) {
            return;
        }
    }
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
    AbortReason abort_reason = ABORT_NONE;
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
            abort_reason = ABORT_CANCELLED;
            break;
        }

        printer_state_lock(state);
        JobStatus status = state->job.status;
        printer_state_unlock(state);
        if (status != JOB_STATUS_PRINTING) {
            /* Job was cancelled (or otherwise moved on) through some
             * other path while we were between lines. */
            abort_reason = ABORT_CANCELLED;
            break;
        }

        if (driver->send_gcode_line(driver, line) != 0) {
            /* The printer didn't acknowledge this line. Treat that as
             * fatal for this print rather than silently skipping ahead —
             * silently dropping a motion or temperature command could
             * leave a real printer in a genuinely bad physical state
             * (head crashed into the bed, heater left on, ...). */
            abort_reason = ABORT_DRIVER_FAILURE;
            break;
        }

        sent_lines++;
        printer_state_lock(state);
        state->job.progress_percent = ((double)sent_lines / (double)total_lines) * 100.0;
        printer_state_unlock(state);
    }

    fclose(file);

    if (abort_reason != ABORT_NONE) {
        /* Stopped early, one way or another — don't leave the printer
         * cooking the part it was half-way through. */
        send_abort_safety_sequence(driver);
    }

    printer_state_lock(state);
    if (abort_reason == ABORT_NONE) {
        state->job.progress_percent = 100.0;
        /* "Finished" printing: go back to READY rather than IDLE, so the
         * same file is still queued and could be printed again without
         * re-uploading it. */
        state->job.status = JOB_STATUS_READY;
    } else if (abort_reason == ABORT_DRIVER_FAILURE) {
        /* Nothing else has touched the job status on this path (unlike a
         * cancel, where job_manager_cancel_print already set
         * JOB_STATUS_IDLE), so without this the job would sit at
         * JOB_STATUS_PRINTING forever with a frozen progress bar and no
         * thread behind it. READY — the same place a completed print
         * lands — is the recoverable choice: the file stays queued so it
         * can simply be started again once the printer is back, and the
         * frontend already knows how to display that status. The
         * progress percentage is deliberately left where it stopped, as
         * a hint of how far the print got before the printer went away. */
        state->job.status = JOB_STATUS_READY;
    }
    /* Cancelled: leave state->job exactly as whatever cancelled it left
     * it (job_manager_cancel_print already set JOB_STATUS_IDLE). */
    printer_state_unlock(state);

    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.cancel_requested = 0;
    pthread_mutex_unlock(&s_streamer.mutex);

    free(args);
    return NULL;
}

/* True if a print is currently running or paused — i.e. the streamer
 * thread is alive and reading a file that the caller must not pull out
 * from under it. Same guard job_manager_delete_file has always had;
 * upload and select need it just as much (see their call sites). */
static int print_is_active(PrinterState *state) {
    printer_state_lock(state);
    int active =
        (state->job.status == JOB_STATUS_PRINTING || state->job.status == JOB_STATUS_PAUSED);
    printer_state_unlock(state);
    return active;
}

int job_manager_save_upload(PrinterState *state, const char *uploads_dir, const char *filename,
                            const char *body, size_t body_len) {
    if (!is_safe_filename(filename)) {
        return -1;
    }

    /* Checked BEFORE anything is written to disk, not just before the
     * job fields are updated: an upload of the same name would truncate
     * the very file the streamer thread currently has open and is
     * reading from, and the rest of the print would be streamed from
     * whatever the new bytes happen to be. (Strictly this is a
     * check-then-act race, but a print can only ever start from
     * JOB_STATUS_READY via job_manager_start_print, so the window can't
     * be hit by anything except two requests racing each other by
     * microseconds — and the job-status assignment below is the real
     * point of no return either way.) */
    if (print_is_active(state)) {
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
    /* The READY -> PRINTING transition happens here, in one locked step,
     * rather than further down once everything else has succeeded. Two
     * "Start" requests arriving together (an impatient double-click is
     * enough — civetweb serves them on separate threads) would otherwise
     * both see READY, both pass this check, and both launch a streamer
     * thread: two threads streaming the same file into the same printer,
     * interleaved line by line. Claiming the status up front makes the
     * second one fail cleanly instead. Every failure path below puts it
     * back to READY.
     *
     * This also has to be true for the join further down to be safe —
     * that assumes at most one streamer thread exists at a time. */
    printer_state_lock(state);

    if (state->job.status != JOB_STATUS_READY) {
        printer_state_unlock(state);
        return -1; /* nothing queued, or already printing */
    }

    char path[512];
    build_upload_path(path, sizeof(path), uploads_dir, state->job.filename);
    state->job.status = JOB_STATUS_PRINTING;
    state->job.progress_percent = 0.0;

    printer_state_unlock(state);

    /* Confirm the file is actually still there before committing to
     * "printing" it — it could in principle have been removed from disk
     * since it was uploaded. stat() outside the lock is fine: we're not
     * touching shared state here, just checking the filesystem. */
    struct stat file_info;
    if (stat(path, &file_info) != 0) {
        printer_state_lock(state);
        state->job.status = JOB_STATUS_READY;
        printer_state_unlock(state);
        return -1;
    }

    StreamerArgs *args = malloc(sizeof(StreamerArgs));
    if (args == NULL) {
        printer_state_lock(state);
        state->job.status = JOB_STATUS_READY;
        printer_state_unlock(state);
        return -1;
    }
    args->state = state;
    args->driver = driver;
    strncpy(args->path, path, sizeof(args->path) - 1);
    args->path[sizeof(args->path) - 1] = '\0';

    /* Reap the previous print's thread, if there was one. The handle is
     * copied out and thread_valid cleared while holding the mutex, and
     * the join then happens with the mutex RELEASED — the same pattern
     * job_manager_shutdown() uses, and for the same reason: the streamer
     * thread takes this very mutex on every loop iteration and again on
     * its way out, so joining it while holding the mutex is a guaranteed
     * deadlock the moment that thread is still alive (which it can be —
     * it keeps running its abort safety sequence for a moment after a
     * cancel). Blocking here is correct: only one streamer thread may
     * exist at a time, so a new print genuinely has to wait for the old
     * one to be finished. */
    pthread_mutex_lock(&s_streamer.mutex);
    int has_previous_thread = s_streamer.thread_valid;
    pthread_t previous_thread = s_streamer.thread;
    s_streamer.thread_valid = 0;
    pthread_mutex_unlock(&s_streamer.mutex);

    if (has_previous_thread) {
        pthread_join(previous_thread, NULL);
    }

    /* Cleared only after the join, and only after the job status is
     * already PRINTING. Both orderings matter: clearing it before the
     * join would let the thread we're waiting on look up, see a job
     * that's PRINTING again and no cancel pending, and carry on
     * streaming its old file — the flag is the only thing that
     * distinguishes "you were cancelled" from "a new print started". */
    pthread_mutex_lock(&s_streamer.mutex);
    s_streamer.cancel_requested = 0;
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

    /* Selecting a different file mid-print would silently kill the print:
     * the streamer thread watches state->job.status and treats "not
     * PRINTING any more" as "someone cancelled me", so the READY set
     * below would stop the print dead with the heaters still on and no
     * indication in the UI that anything happened. */
    if (print_is_active(state)) {
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
