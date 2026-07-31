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

    PrinterHeater heater;
    double max_target;
    if (strcmp(heater_item->valuestring, "hotend") == 0) {
        heater = PRINTER_HEATER_HOTEND;
        max_target = MAX_HOTEND_TARGET_C;
    } else if (strcmp(heater_item->valuestring, "bed") == 0) {
        heater = PRINTER_HEATER_BED;
        max_target = MAX_BED_TARGET_C;
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
        cJSON_Delete(json);

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
     * a form field. mg_get_var handles the URL-decoding for us. */
    char filename[256];
    int filename_len = -1;
    if (req_info->query_string != NULL) {
        filename_len = mg_get_var(req_info->query_string, strlen(req_info->query_string),
                                  "filename", filename, sizeof(filename));
    }
    if (filename_len <= 0) {
        mg_send_http_error(conn, 400, "Missing ?filename=... query parameter");
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
        mg_send_http_error(conn, 400, "Could not save upload (invalid filename or disk error)");
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

    if (job_manager_start_print(ctx->state, ctx->uploads_dir) != 0) {
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

/* ---------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------- */

void api_handlers_register_all(struct mg_context *ctx, AppContext *app_context) {
    mg_set_request_handler(ctx, "/api/state", state_handler, app_context);
    mg_set_request_handler(ctx, "/api/jog", jog_handler, app_context);
    mg_set_request_handler(ctx, "/api/home", home_handler, app_context);
    mg_set_request_handler(ctx, "/api/temp", temp_handler, app_context);
    mg_set_request_handler(ctx, "/api/profile", profile_handler, app_context);
    mg_set_request_handler(ctx, "/api/upload", upload_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/start", print_start_handler, app_context);
    mg_set_request_handler(ctx, "/api/print/cancel", print_cancel_handler, app_context);
}
