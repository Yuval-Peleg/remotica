/*
 * api_handlers.c
 * ===============
 * See api_handlers.h for the overview. Each handler function follows the
 * same rough shape:
 *   1. Check the HTTP method is what we expect.
 *   2. For POST requests with a body, read + parse the JSON.
 *   3. Validate the input (reject anything that doesn't make sense).
 *   4. Do the actual work (usually: call into the driver, job manager, or
 *      printer state).
 *   5. Send a JSON response.
 *
 * civetweb request handler functions all have the same required
 * signature: `int handler(struct mg_connection *conn, void *cbdata)`. We
 * always pass an `AppContext *` as cbdata (see api_handlers_register_all
 * at the bottom of this file), so the first line of every handler here is
 * getting that context back out.
 */

#include "api_handlers.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "civetweb.h"
#include "printer_database.h"
#include "system_info.h"

/* Maximum size we'll accept for a single JSON request body (jog/temp/
 * profile requests are tiny — a few dozen bytes — so this is generous
 * headroom, not a tight limit). Uploaded gcode files go through a
 * separate, much larger limit — see upload_handler below. */
#define MAX_JSON_BODY_BYTES 8192

/* Uploaded files can legitimately be several megabytes (a detailed print
 * can be a long gcode file). 64MB is a generous cap for a prototype —
 * big enough for any realistic single-part print, small enough that a
 * client can't fill up the disk by "uploading" gigabytes. */
#define MAX_UPLOAD_BYTES (64 * 1024 * 1024)

/* ---------------------------------------------------------------------
 * Small shared helpers used by several handlers below
 * --------------------------------------------------------------------- */

/* Reads the request body (up to MAX_JSON_BODY_BYTES) and parses it as
 * JSON. Returns the parsed cJSON object (caller must cJSON_Delete it), or
 * NULL if the body was empty, too large, or not valid JSON — in which
 * case this has already sent an error response, so the caller should
 * just `return 1` immediately without sending anything else. */
static cJSON *read_json_body(struct mg_connection *conn) {
    char body[MAX_JSON_BODY_BYTES];
    int body_len = mg_read(conn, body, sizeof(body) - 1);
    if (body_len <= 0) {
        mg_send_http_error(conn, 400, "Expected a JSON request body");
        return NULL;
    }
    body[body_len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        mg_send_http_error(conn, 400, "Body must be valid JSON");
        return NULL;
    }
    return json;
}

/* Sends `json` as a "200 OK" response with Content-Type: application/json,
 * then deletes it (the caller is done with it after this call — treat
 * this function as taking ownership of the object). */
static void send_json_and_delete(struct mg_connection *conn, cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (text == NULL) {
        mg_send_http_error(conn, 500, "Failed to build response");
        return;
    }

    mg_send_http_ok(conn, "application/json", (long long)strlen(text));
    mg_write(conn, text, strlen(text));
    free(text);
}

/* Sends a simple {"ok":true} response — used by endpoints that don't have
 * any real data to report back, just success/failure (home, print/start,
 * print/cancel). */
static void send_ok(struct mg_connection *conn) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", 1);
    send_json_and_delete(conn, json);
}

/* Clamps `value` into [min, max]. Used to keep jog targets inside the
 * printer's physical build volume, and temperature targets inside sane
 * limits, before anything is sent to the driver. */
static double clamp(double value, double min, double max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/* True while a print is running or paused. Several endpoints have to
 * refuse outright in that situation rather than do something dangerous
 * to a print already in progress — see each call site below for what
 * specifically goes wrong. job_manager.c enforces the same rule
 * internally for the ones that go through it; checking here as well is
 * deliberate duplication, so the client gets an honest 409 ("that
 * conflicts with the printer's current state") instead of a misleading
 * "file not found"-shaped error. */
static int print_is_active(PrinterState *state) {
    printer_state_lock(state);
    int active =
        (state->job.status == JOB_STATUS_PRINTING || state->job.status == JOB_STATUS_PAUSED);
    printer_state_unlock(state);
    return active;
}

/* True once a human has confirmed the printer's physical profile — either
 * directly in Settings, or indirectly via a successful firmware
 * auto-match (see main.c's try_auto_detect_profile). False means the
 * profile is still on hardcoded defaults, which are a plausible-looking
 * guess, not a fact about the printer actually attached — sending real
 * jog/home/print commands against made-up bed/Z limits risks crashing
 * into a frame those numbers don't actually describe. */
static int profile_is_configured(const PrinterProfile *profile) {
    return profile->source != PRINTER_PROFILE_SOURCE_DEFAULT;
}

/* Reads the "filename" query string parameter (e.g. from
 * "?filename=benchy.gcode") into `out`. Returns 1 on success, or 0 and
 * sends a 400 error itself if it's missing — so callers can just
 * `if (!get_filename_query_param(...)) return 1;`. Used by every handler
 * that identifies a file by name in the URL rather than the body. */
static int get_filename_query_param(struct mg_connection *conn, char *out, size_t out_size) {
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    int len = -1;
    if (req_info->query_string != NULL) {
        len = mg_get_var(req_info->query_string, strlen(req_info->query_string), "filename", out,
                         out_size);
    }

    if (len <= 0) {
        mg_send_http_error(conn, 400, "Missing ?filename=... query parameter");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/state
 * --------------------------------------------------------------------- */

static int state_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    cJSON *json = printer_state_to_json(ctx->state);
    send_json_and_delete(conn, json);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/jog   body: {"axis":"X","deltaMm":1.0}
 * --------------------------------------------------------------------- */

static int jog_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    /* Jogging during a print isn't just "a move at a bad time": a jog is
     * sent as G91 / G1 / G90, and those lines are interleaved into the
     * same single command stream the print file is being fed through. If
     * one lands between two lines of the file, the file's own modal
     * positioning mode is silently replaced — the next line, written to
     * mean "move to X150", is then obeyed as "move 150mm from here", and
     * the head takes off across the bed at layer height (or the extruder
     * is asked for several thousand millimetres of filament). Refuse
     * instead. Temperature changes stay allowed: those are a normal,
     * legitimate thing to do mid-print. */
    if (print_is_active(ctx->state)) {
        mg_send_http_error(conn, 409, "Cannot jog while a print is in progress");
        return 1;
    }

    if (!profile_is_configured(ctx->profile)) {
        mg_send_http_error(conn, 409, "Cannot jog without a configured printer profile");
        return 1;
    }

    cJSON *json = read_json_body(conn);
    if (json == NULL) {
        return 1; /* read_json_body already sent an error response */
    }

    cJSON *axis_item = cJSON_GetObjectItemCaseSensitive(json, "axis");
    cJSON *delta_item = cJSON_GetObjectItemCaseSensitive(json, "deltaMm");

    if (!cJSON_IsString(axis_item) || strlen(axis_item->valuestring) != 1 ||
        !cJSON_IsNumber(delta_item)) {
        cJSON_Delete(json);
        mg_send_http_error(conn, 400, "Expected {\"axis\":\"X\",\"deltaMm\":1.0}");
        return 1;
    }

    char axis = axis_item->valuestring[0];
    double delta_mm = delta_item->valuedouble;
    cJSON_Delete(json);

    if (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'E') {
        mg_send_http_error(conn, 400, "axis must be one of X, Y, Z, E");
        return 1;
    }

    /* E (the extruder) is a special case: real printers refuse to
     * extrude cold filament (it can jam or strip the drive gear), so we
     * enforce the same rule the frontend's ControlPanel.jsx already does
     * client-side — this is a second, backend-side copy of that same
     * safety check, which matters because a backend should never trust a
     * frontend-only check for something with a physical consequence. */
    if (axis == 'E') {
        printer_state_lock(ctx->state);
        double hotend_current = ctx->state->hotend.current_c;
        printer_state_unlock(ctx->state);

        if (hotend_current < ctx->profile->min_extrude_temp_c) {
            mg_send_http_error(conn, 409, "Hotend is too cold to extrude (below %.0fC)",
                               ctx->profile->min_extrude_temp_c);
            return 1;
        }

        if (ctx->driver->jog(ctx->driver, axis, delta_mm) != 0) {
            mg_send_http_error(conn, 502, "Driver failed to jog E");
            return 1;
        }
        send_ok(conn);
        return 1;
    }

    /* X, Y, and Z all get clamped to the printer's physical build volume
     * before we ever send a move — the driver itself doesn't do this
     * (see the comment in transport_sim.c's sim_jog), so it's this
     * layer's job. We read the current position, compute where the move
     * WOULD end up, clamp that, and send only the clamped delta. */
    printer_state_lock(ctx->state);
    double current;
    double min_limit = 0.0;
    double max_limit;
    switch (axis) {
    case 'X':
        current = ctx->state->position.x_mm;
        max_limit = ctx->profile->bed_width_mm;
        break;
    case 'Y':
        current = ctx->state->position.y_mm;
        max_limit = ctx->profile->bed_depth_mm;
        break;
    default: /* 'Z' */
        current = ctx->state->position.z_mm;
        max_limit = ctx->profile->max_z_mm;
        break;
    }
    printer_state_unlock(ctx->state);

    double clamped_target = clamp(current + delta_mm, min_limit, max_limit);
    double clamped_delta = clamped_target - current;

    if (ctx->driver->jog(ctx->driver, axis, clamped_delta) != 0) {
        mg_send_http_error(conn, 502, "Driver failed to jog %c", axis);
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/home
 * --------------------------------------------------------------------- */

static int home_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    /* Homing mid-print is worse than jogging mid-print: G28 drives the
     * head to the endstops at homing speed on a path that goes straight
     * through whatever is being printed, and afterwards the print file's
     * remaining coordinates no longer mean what they did. */
    if (print_is_active(ctx->state)) {
        mg_send_http_error(conn, 409, "Cannot home while a print is in progress");
        return 1;
    }

    if (!profile_is_configured(ctx->profile)) {
        mg_send_http_error(conn, 409, "Cannot home without a configured printer profile");
        return 1;
    }

    if (ctx->driver->home(ctx->driver) != 0) {
        mg_send_http_error(conn, 502, "Driver failed to home");
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/temp   body: {"heater":"hotend","celsius":210}
 * --------------------------------------------------------------------- */

/* Sane upper bounds for target temperatures, independent of whatever a
 * client asks for — matches the frontend's TempDial `maxTarget={120}` for
 * the bed, and a generous-but-not-silly ceiling for the hotend. These
 * exist so a typo or a bug elsewhere can't ask the printer to try to
 * reach an absurd, unsafe temperature. */
#define MAX_HOTEND_TARGET_C 300.0
#define MAX_BED_TARGET_C 120.0

static int temp_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    cJSON *json = read_json_body(conn);
    if (json == NULL) {
        return 1;
    }

    cJSON *heater_item = cJSON_GetObjectItemCaseSensitive(json, "heater");
    cJSON *celsius_item = cJSON_GetObjectItemCaseSensitive(json, "celsius");

    if (!cJSON_IsString(heater_item) || !cJSON_IsNumber(celsius_item)) {
        cJSON_Delete(json);
        mg_send_http_error(conn, 400, "Expected {\"heater\":\"hotend\"|\"bed\",\"celsius\":N}");
        return 1;
    }

    /* The profile's max*TempC is per-printer (or per-material) and
     * user-editable from the Settings page, but it's still clamped
     * against these hardcoded absolute ceilings — a mistyped profile
     * value (or one copied from the wrong printer database entry)
     * shouldn't be able to ask real hardware to exceed them either. */
    PrinterHeater heater;
    double max_target;
    if (strcmp(heater_item->valuestring, "hotend") == 0) {
        heater = PRINTER_HEATER_HOTEND;
        max_target = clamp(ctx->profile->max_hotend_temp_c, 0.0, MAX_HOTEND_TARGET_C);
    } else if (strcmp(heater_item->valuestring, "bed") == 0) {
        heater = PRINTER_HEATER_BED;
        max_target = clamp(ctx->profile->max_bed_temp_c, 0.0, MAX_BED_TARGET_C);
    } else {
        cJSON_Delete(json);
        mg_send_http_error(conn, 400, "heater must be \"hotend\" or \"bed\"");
        return 1;
    }

    double celsius = clamp(celsius_item->valuedouble, 0.0, max_target);
    cJSON_Delete(json);

    if (ctx->driver->set_target_temp(ctx->driver, heater, celsius) != 0) {
        mg_send_http_error(conn, 502, "Driver failed to set target temperature");
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/profile,  POST /api/profile
 * --------------------------------------------------------------------- */

static int profile_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") == 0) {
        send_json_and_delete(conn, printer_profile_to_json(ctx->profile));
        return 1;
    }

    if (strcmp(req_info->request_method, "POST") == 0) {
        cJSON *json = read_json_body(conn);
        if (json == NULL) {
            return 1;
        }

        /* Partial update: only fields present in the request body are
         * changed (see printer_profile_from_json's doc comment). */
        printer_profile_from_json(ctx->profile, json);
        int name_supplied = cJSON_HasObjectItem(json, "printerName");
        cJSON_Delete(json);

        /* A human just explicitly confirmed this profile via Settings —
         * always mark it "manual" regardless of what was there before
         * (even if it was "auto"). Unconditional on purpose: a client
         * can't opt out of this by sending its own "source" in the body,
         * since this runs after the merge and overrides whatever
         * from_json may have set.
         *
         * The name is only kept when the client actually sent one (the
         * preset it picked, or a name typed for a hand-entered profile).
         * A save with no name clears it rather than leaving a previous
         * auto-detected model name attached to numbers it no longer
         * describes. */
        ctx->profile->source = PRINTER_PROFILE_SOURCE_MANUAL;
        if (!name_supplied) {
            ctx->profile->printer_name[0] = '\0';
        }

        if (printer_profile_save(ctx->profile, ctx->profile_path) != 0) {
            mg_send_http_error(conn, 500, "Failed to save profile to disk");
            return 1;
        }

        send_json_and_delete(conn, printer_profile_to_json(ctx->profile));
        return 1;
    }

    mg_send_http_error(conn, 405, "Use GET or POST");
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/printer-database
 * --------------------------------------------------------------------- */

static int printer_database_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata; /* the database is a fixed static table, no AppContext needed */
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "printers", printer_database_to_json());
    send_json_and_delete(conn, root);
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/printer-suggestion
 * --------------------------------------------------------------------- */

/* What the currently-attached printer's own firmware reply suggests it is,
 * computed fresh on every request. Deliberately separate from the profile:
 * main.c's auto-detect only ever writes a match into the profile when
 * nothing has been configured yet, so once a human has saved anything the
 * profile stops reflecting what the hardware is reporting. Settings needs
 * both — "here's what Remotica thinks is plugged in" alongside "here's
 * what's actually configured" — so this reports the raw suggestion
 * regardless of what's saved, and changes nothing. */
static int printer_suggestion_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    printer_state_lock(ctx->state);
    int connected = ctx->state->connected;
    char firmware_info[sizeof(ctx->state->firmware_info)];
    snprintf(firmware_info, sizeof(firmware_info), "%s", ctx->state->firmware_info);
    printer_state_unlock(ctx->state);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "firmwareInfo", firmware_info);

    const PrinterDatabaseEntry *match = printer_database_match_firmware(firmware_info);
    if (match == NULL) {
        cJSON_AddNullToObject(root, "match");
    } else {
        /* Same shape as one entry of GET /api/printer-database, so the
         * frontend can hand it to the same "apply this preset" code path
         * with no special-casing. */
        cJSON *item = printer_profile_to_json(&match->profile);
        cJSON_AddStringToObject(item, "id", match->id);
        cJSON_AddStringToObject(item, "name", match->name);
        cJSON_AddItemToObject(root, "match", item);
    }

    send_json_and_delete(conn, root);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/upload?filename=whatever.gcode   body: raw file bytes
 * --------------------------------------------------------------------- */

static int upload_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    /* The filename is passed as a query string parameter, e.g.
     * "POST /api/upload?filename=benchy.gcode" — simpler than parsing a
     * multipart/form-data body for a single file, at the cost of the
     * frontend needing to URL-encode the filename into the URL instead of
     * a form field. */
    char filename[256];
    if (!get_filename_query_param(conn, filename, sizeof(filename))) {
        return 1;
    }

    /* Rejected up front rather than after reading the (potentially
     * multi-megabyte) body — job_manager_save_upload refuses too, but
     * there's no reason to spend the transfer first. See its doc comment
     * for why an upload mid-print is unsafe. */
    if (print_is_active(ctx->state)) {
        mg_send_http_error(conn, 409, "Cannot upload a file while a print is in progress");
        return 1;
    }

    long long content_length = req_info->content_length;
    if (content_length <= 0 || content_length > MAX_UPLOAD_BYTES) {
        mg_send_http_error(conn, 400, "Missing, empty, or too-large request body (limit %d bytes)",
                           MAX_UPLOAD_BYTES);
        return 1;
    }

    char *body = malloc((size_t)content_length);
    if (body == NULL) {
        mg_send_http_error(conn, 500, "Out of memory reading upload");
        return 1;
    }

    /* mg_read (like the underlying read() syscall it wraps) is allowed to
     * return fewer bytes than we asked for in a single call, so this
     * loops until we've either read everything or hit an error/EOF. */
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        int n = mg_read(conn, body + total_read, (size_t)content_length - total_read);
        if (n <= 0) {
            break;
        }
        total_read += (size_t)n;
    }

    if (total_read != (size_t)content_length) {
        free(body);
        mg_send_http_error(conn, 400, "Upload body was shorter than its Content-Length");
        return 1;
    }

    int save_result =
        job_manager_save_upload(ctx->state, ctx->uploads_dir, filename, body, total_read);
    free(body);

    if (save_result != 0) {
        mg_send_http_error(conn, 400,
                           "Could not save upload (invalid filename, disk error, or a print "
                           "started in the meantime)");
        return 1;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", 1);
    cJSON_AddStringToObject(response, "filename", filename);
    cJSON_AddNumberToObject(response, "size", (double)total_read);
    send_json_and_delete(conn, response);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/print/start,  POST /api/print/cancel
 * --------------------------------------------------------------------- */

static int print_start_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    /* A multi-hour, unattended print is the highest-stakes case for
     * trusting the profile's numbers — same reasoning as jog/home being
     * refused below JOB_STATUS_READY, just checked here since
     * job_manager_start_print() has no existing concept of PrinterProfile
     * and profile-safety checks already live at this layer elsewhere
     * (see jog_handler's cold-extrusion check above). */
    if (!profile_is_configured(ctx->profile)) {
        mg_send_http_error(conn, 409, "Cannot start a print without a configured printer profile");
        return 1;
    }

    if (job_manager_start_print(ctx->state, ctx->driver, ctx->uploads_dir) != 0) {
        mg_send_http_error(conn, 409, "No file ready to print");
        return 1;
    }

    send_ok(conn);
    return 1;
}

static int print_cancel_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    job_manager_cancel_print(ctx->state);
    send_ok(conn);
    return 1;
}

static int print_pause_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    if (job_manager_pause_print(ctx->state) != 0) {
        mg_send_http_error(conn, 409, "Not currently printing");
        return 1;
    }

    send_ok(conn);
    return 1;
}

static int print_resume_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    if (job_manager_resume_print(ctx->state) != 0) {
        mg_send_http_error(conn, 409, "Not currently paused");
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/files                        — list every gcode file on disk
 * GET /api/files/content?filename=...   — raw bytes of one file
 * POST /api/files/select?filename=...   — queue an existing file to print
 * POST /api/files/delete?filename=...   — remove a file from disk
 *
 * Together these back the frontend's "on device" file browser: every
 * file ever uploaded stays in uploads_dir (see job_manager.h) so it can
 * be reprinted later without uploading it again.
 *
 * Note on GET /api/files/content: rather than parsing gcode thumbnails
 * and print-time estimates in C, this just hands back the raw file text
 * and lets the frontend reuse the parsers it already has
 * (src/lib/gcode-thumbnail.js, src/lib/gcode-print-time.js) — one less
 * place that logic has to be written and kept correct.
 * --------------------------------------------------------------------- */

/* How many files the "on device" browser can show at once. A generous
 * limit for a prototype — if this is ever hit in practice, paging the
 * list would be a better fix than just raising the number. */
#define MAX_LISTED_FILES 200

static int files_list_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    GcodeFileInfo files[MAX_LISTED_FILES];
    int count = job_manager_list_files(ctx->uploads_dir, files, MAX_LISTED_FILES);

    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "filename", files[i].filename);
        cJSON_AddNumberToObject(entry, "size", (double)files[i].size_bytes);
        cJSON_AddNumberToObject(entry, "modifiedAt", (double)files[i].modified_unix_time);
        cJSON_AddItemToArray(array, entry);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "files", array);
    send_json_and_delete(conn, response);
    return 1;
}

static int files_content_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    char filename[256];
    if (!get_filename_query_param(conn, filename, sizeof(filename))) {
        return 1;
    }

    /* Deliberately uses job_manager_file_exists() (read-only) rather than
     * job_manager_select_existing() here — this route is for previewing
     * a file's contents (to build a thumbnail/summary in the "on device"
     * browser), which the frontend may do for every listed file just by
     * having the browser open. That must NOT have the side effect of
     * changing which file is actually queued to print. */
    if (!job_manager_file_exists(ctx->uploads_dir, filename)) {
        mg_send_http_error(conn, 404, "File not found");
        return 1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ctx->uploads_dir, filename);
    mg_send_file(conn, path);
    return 1;
}

static int files_select_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    char filename[256];
    if (!get_filename_query_param(conn, filename, sizeof(filename))) {
        return 1;
    }

    /* Selecting a different file mid-print would stop the running print
     * (see job_manager_select_existing) — and this endpoint is one click
     * in the "on device" file browser, so it's an easy mistake to make. */
    if (print_is_active(ctx->state)) {
        mg_send_http_error(conn, 409, "Cannot select a file while a print is in progress");
        return 1;
    }

    if (job_manager_select_existing(ctx->state, ctx->uploads_dir, filename) != 0) {
        mg_send_http_error(conn, 404, "File not found");
        return 1;
    }

    send_ok(conn);
    return 1;
}

static int files_delete_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Use POST");
        return 1;
    }

    char filename[256];
    if (!get_filename_query_param(conn, filename, sizeof(filename))) {
        return 1;
    }

    if (job_manager_delete_file(ctx->state, ctx->uploads_dir, filename) != 0) {
        mg_send_http_error(conn, 409, "Could not delete file (not found, or currently printing)");
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * GET /api/system
 *
 * Host-level status, not printer status — what build is running, how
 * long it's been up, where its data lives, and whether it's an installed
 * service that will come back after a power cut.
 * --------------------------------------------------------------------- */

static int system_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Only GET is supported here");
        return 1;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "version", REMOTICA_VERSION);
    cJSON_AddNumberToObject(json, "uptimeSeconds", (double)(time(NULL) - ctx->started_at));
    cJSON_AddStringToObject(json, "dataDir", ctx->data_dir);
    cJSON_AddNumberToObject(json, "dataFreeBytes", (double)system_info_free_bytes(ctx->data_dir));
    cJSON_AddStringToObject(json, "serialPort",
                            ctx->serial_device != NULL ? ctx->serial_device : "simulator");
    cJSON_AddBoolToObject(json, "servingFrontend", ctx->web_root != NULL);

    /* "managed" is what the frontend keys off: false means this is a
     * source checkout with no systemd unit, so there is genuinely
     * nothing to toggle and the UI shows a disabled control with an
     * explanation rather than one that silently fails. bootStartEnabled
     * is omitted entirely (rather than sent as false) when it couldn't
     * be determined, so the client can't mistake "unknown" for "off". */
    int managed = system_info_is_managed();
    cJSON_AddBoolToObject(json, "managed", managed);
    if (managed) {
        int enabled = system_info_boot_start_enabled();
        if (enabled >= 0) {
            cJSON_AddBoolToObject(json, "bootStartEnabled", enabled);
        }
    }

    send_json_and_delete(conn, json);
    return 1;
}

/* ---------------------------------------------------------------------
 * POST /api/system/boot-start   {"enabled": true|false}
 *
 * The one endpoint that reaches outside Remotica and changes something
 * about the host machine — see system_info.h for how the privilege to
 * do that is granted, and how narrowly.
 * --------------------------------------------------------------------- */

static int boot_start_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Only POST is supported here");
        return 1;
    }

    if (!system_info_is_managed()) {
        mg_send_http_error(conn, 409,
                           "Not running as an installed service, so there is no boot "
                           "behaviour to change");
        return 1;
    }

    cJSON *json = read_json_body(conn);
    if (json == NULL) {
        return 1;
    }

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        mg_send_http_error(conn, 400, "Expected {\"enabled\": true|false}");
        return 1;
    }
    int want_enabled = cJSON_IsTrue(enabled);
    cJSON_Delete(json);

    /* systemctl's own output is forwarded to the client deliberately
     * rather than flattened to "failed". The realistic failure here is a
     * sudoers rule that no longer matches — a renamed unit, a partial
     * install, a distro with systemctl somewhere other than /usr/bin —
     * and that is completely invisible from the UI unless the actual
     * error text comes back. */
    char err[256];
    if (!system_info_set_boot_start(want_enabled, err, sizeof(err))) {
        mg_send_http_error(conn, 500, "systemctl failed: %s", err[0] != '\0' ? err : "no output");
        return 1;
    }

    send_ok(conn);
    return 1;
}

/* ---------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------- */

void api_handlers_register_all(struct mg_context *ctx, AppContext *app_context) {
    mg_set_request_handler(ctx, "/api/state", state_handler, app_context);
    mg_set_request_handler(ctx, "/api/jog", jog_handler, app_context);
    mg_set_request_handler(ctx, "/api/home", home_handler, app_context);
    mg_set_request_handler(ctx, "/api/temp", temp_handler, app_context);
    mg_set_request_handler(ctx, "/api/profile", profile_handler, app_context);
    mg_set_request_handler(ctx, "/api/printer-database", printer_database_handler, NULL);
    mg_set_request_handler(ctx, "/api/printer-suggestion", printer_suggestion_handler, app_context);
    mg_set_request_handler(ctx, "/api/upload", upload_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/start", print_start_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/cancel", print_cancel_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/pause", print_pause_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/resume", print_resume_handler, app_context);
    mg_set_request_handler(ctx, "/api/files", files_list_handler, app_context);
    mg_set_request_handler(ctx, "/api/files/content", files_content_handler, app_context);
    mg_set_request_handler(ctx, "/api/files/select", files_select_handler, app_context);
    mg_set_request_handler(ctx, "/api/files/delete", files_delete_handler, app_context);
    mg_set_request_handler(ctx, "/api/system", system_handler, app_context);
    mg_set_request_handler(ctx, "/api/system/boot-start", boot_start_handler, app_context);
}
