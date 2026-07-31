/*
 * printer_state.c
 * ================
 * Implementation of the functions declared in printer_state.h. See that
 * file first for the "why" — this file is mostly straightforward
 * "lock, copy fields, unlock" plumbing.
 */

#include "printer_state.h"

#include <string.h>

#include "cJSON.h"

void printer_state_init(PrinterState *state) {
    /* memset zeroes everything first, so we don't have to explicitly set
     * every numeric field to 0 by hand — it also conveniently makes
     * filename[] an empty string, since an all-zero char array starts
     * with a '\0' at position 0. */
    memset(state, 0, sizeof(*state));

    pthread_mutex_init(&state->lock, NULL);

    state->connected = 0;
    strncpy(state->connection_type, "usb", sizeof(state->connection_type) - 1);

    state->job.status = JOB_STATUS_IDLE;
}

void printer_state_lock(PrinterState *state) {
    pthread_mutex_lock(&state->lock);
}

void printer_state_unlock(PrinterState *state) {
    pthread_mutex_unlock(&state->lock);
}

/* Turns the JobStatus enum into the same lowercase words the frontend
 * already uses ("idle" / "ready" / "printing"), so the two sides agree on
 * vocabulary without the frontend needing to know about our C enum. */
static const char *job_status_to_string(JobStatus status) {
    switch (status) {
    case JOB_STATUS_IDLE:
        return "idle";
    case JOB_STATUS_READY:
        return "ready";
    case JOB_STATUS_PRINTING:
        return "printing";
    case JOB_STATUS_PAUSED:
        return "paused";
    default:
        /* Should never happen, but every switch on an enum should
         * have a fallback so we don't return garbage if someone adds
         * a new JobStatus value later and forgets to update this. */
        return "unknown";
    }
}

cJSON *printer_state_to_json(PrinterState *state) {
    /* Step 1: grab a private copy of everything we need while holding the
     * lock, then unlock as fast as possible. Building JSON involves a lot
     * of small memory allocations, which is "slow" compared to just
     * copying a few numbers — we don't want to make every other thread
     * wait on the mutex while we do that slow work. */
    printer_state_lock(state);

    int connected = state->connected;
    char connection_type[sizeof(state->connection_type)];
    strncpy(connection_type, state->connection_type, sizeof(connection_type));
    char firmware_info[sizeof(state->firmware_info)];
    strncpy(firmware_info, state->firmware_info, sizeof(firmware_info));

    TempReading hotend = state->hotend;
    TempReading bed = state->bed;
    PrinterPosition position = state->position;

    JobStatus job_status = state->job.status;
    char filename[sizeof(state->job.filename)];
    strncpy(filename, state->job.filename, sizeof(filename));
    double progress_percent = state->job.progress_percent;

    printer_state_unlock(state);

    /* Step 2: now that we're unlocked, build the JSON object from our
     * local copies. This shape is what the frontend receives from both
     * GET /api/state and the WebSocket push — see CLAUDE.md's "Planned
     * backend" section for the API shape this is implementing. */
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "connectionType", connection_type);
    cJSON_AddStringToObject(root, "firmwareInfo", firmware_info);

    cJSON *hotend_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(hotend_json, "current", hotend.current_c);
    cJSON_AddNumberToObject(hotend_json, "target", hotend.target_c);
    cJSON_AddItemToObject(root, "hotend", hotend_json);

    cJSON *bed_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(bed_json, "current", bed.current_c);
    cJSON_AddNumberToObject(bed_json, "target", bed.target_c);
    cJSON_AddItemToObject(root, "bed", bed_json);

    cJSON *position_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(position_json, "x", position.x_mm);
    cJSON_AddNumberToObject(position_json, "y", position.y_mm);
    cJSON_AddNumberToObject(position_json, "z", position.z_mm);
    cJSON_AddNumberToObject(position_json, "e", position.e_mm);
    cJSON_AddItemToObject(root, "position", position_json);

    cJSON *job_json = cJSON_CreateObject();
    cJSON_AddStringToObject(job_json, "status", job_status_to_string(job_status));
    cJSON_AddStringToObject(job_json, "filename", filename);
    cJSON_AddNumberToObject(job_json, "progress", progress_percent);
    cJSON_AddItemToObject(root, "job", job_json);

    return root;
}
