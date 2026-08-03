/*
 * printer_profile.c
 * ==================
 * See printer_profile.h for the "why". This file is the "how": reading
 * and writing a small JSON file, plus converting to/from cJSON objects.
 */

#include "printer_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

void printer_profile_defaults(PrinterProfile *profile) {
    /* These numbers match frontend/src/lib/printer-profile.js exactly —
     * keep them in sync if that file's placeholder values ever change,
     * until the frontend switches over to reading this from the backend
     * instead of hardcoding it. */
    profile->bed_width_mm = 220.0;
    profile->bed_depth_mm = 220.0;
    profile->max_z_mm = 250.0;
    profile->min_extrude_temp_c = 170.0;
    /* These two match the frontend's previous hardcoded TempDial ceilings
     * exactly, so nothing about the dials' behavior changes until someone
     * actually edits these in the Settings page. */
    profile->max_hotend_temp_c = 280.0;
    profile->max_bed_temp_c = 120.0;

    profile->source = PRINTER_PROFILE_SOURCE_DEFAULT;
    profile->printer_name[0] = '\0';
}

static const char *profile_source_to_string(PrinterProfileSource source) {
    switch (source) {
    case PRINTER_PROFILE_SOURCE_AUTO:
        return "auto";
    case PRINTER_PROFILE_SOURCE_MANUAL:
        return "manual";
    default:
        return "default";
    }
}

/* Returns 1 and writes *out if str is a recognized value, else 0 and leaves
 * *out untouched - same "only overwrite on a valid value" contract as
 * read_number_field() below, so an unrecognized string in profile.json
 * doesn't silently corrupt the field. */
static int profile_source_from_string(const char *str, PrinterProfileSource *out) {
    if (strcmp(str, "auto") == 0) {
        *out = PRINTER_PROFILE_SOURCE_AUTO;
        return 1;
    }
    if (strcmp(str, "manual") == 0) {
        *out = PRINTER_PROFILE_SOURCE_MANUAL;
        return 1;
    }
    if (strcmp(str, "default") == 0) {
        *out = PRINTER_PROFILE_SOURCE_DEFAULT;
        return 1;
    }
    return 0;
}

/* Small helper: given a JSON object and a field name, if that field
 * exists AND is a number, write it into *out and return 1. Otherwise
 * leave *out untouched and return 0. This is what makes
 * printer_profile_from_json() a "partial update" — fields the caller
 * didn't send are simply skipped. */
static int read_number_field(const cJSON *json, const char *field_name, double *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, field_name);
    if (item != NULL && cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return 1;
    }
    return 0;
}

/* Same "only overwrite on a valid value" contract as read_number_field(),
 * for a fixed-size char buffer field. */
static int read_string_field(const cJSON *json, const char *field_name, char *out,
                             size_t out_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, field_name);
    if (item != NULL && cJSON_IsString(item)) {
        snprintf(out, out_size, "%s", item->valuestring);
        return 1;
    }
    return 0;
}

int printer_profile_from_json(PrinterProfile *profile, const cJSON *json) {
    if (json == NULL) {
        return -1;
    }

    /* Each of these is a no-op if the field is missing, so calling this
     * with e.g. just {"maxZMm": 300} only changes max_z_mm. */
    read_number_field(json, "bedWidthMm", &profile->bed_width_mm);
    read_number_field(json, "bedDepthMm", &profile->bed_depth_mm);
    read_number_field(json, "maxZMm", &profile->max_z_mm);
    read_number_field(json, "minExtrudeTempC", &profile->min_extrude_temp_c);
    read_number_field(json, "maxHotendTempC", &profile->max_hotend_temp_c);
    read_number_field(json, "maxBedTempC", &profile->max_bed_temp_c);

    cJSON *source_item = cJSON_GetObjectItemCaseSensitive(json, "source");
    if (source_item != NULL && cJSON_IsString(source_item)) {
        profile_source_from_string(source_item->valuestring, &profile->source);
    }
    /* "detectedName" is the field's old name, kept as a read-only fallback
     * so a profile.json written before the rename still shows its printer
     * name instead of silently going blank. Only consulted when the
     * current name isn't present, and never written back out. */
    if (!read_string_field(json, "printerName", profile->printer_name,
                           sizeof(profile->printer_name))) {
        read_string_field(json, "detectedName", profile->printer_name,
                          sizeof(profile->printer_name));
    }

    return 0;
}

cJSON *printer_profile_to_json(const PrinterProfile *profile) {
    cJSON *root = cJSON_CreateObject();

    /* Field names use camelCase on purpose, to match the JavaScript/JSON
     * convention the frontend already uses (see printer-profile.js) —
     * the C code internally uses snake_case (normal C style), but what
     * goes over the wire matches what the frontend expects. */
    cJSON_AddNumberToObject(root, "bedWidthMm", profile->bed_width_mm);
    cJSON_AddNumberToObject(root, "bedDepthMm", profile->bed_depth_mm);
    cJSON_AddNumberToObject(root, "maxZMm", profile->max_z_mm);
    cJSON_AddNumberToObject(root, "minExtrudeTempC", profile->min_extrude_temp_c);
    cJSON_AddNumberToObject(root, "maxHotendTempC", profile->max_hotend_temp_c);
    cJSON_AddNumberToObject(root, "maxBedTempC", profile->max_bed_temp_c);

    cJSON_AddStringToObject(root, "source", profile_source_to_string(profile->source));
    cJSON_AddStringToObject(root, "printerName", profile->printer_name);

    return root;
}

void printer_profile_load(PrinterProfile *profile, const char *path) {
    /* Always start from the known-good defaults. If loading fails for any
     * reason below, we simply keep these instead of leaving the profile
     * half-initialized. */
    printer_profile_defaults(profile);

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        /* Most common reason: first run, the file doesn't exist yet.
         * That's fine — printer_profile_save() will create it the first
         * time someone changes a setting. */
        return;
    }

    /* Read the whole file into memory. Profile files are tiny (a handful
     * of numbers), so there's no need for anything fancier than one
     * fixed-size buffer. */
    char buffer[4096];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[bytes_read] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json != NULL) {
        printer_profile_from_json(profile, json);

        /* Migration: printer_profile_save() had exactly one call site
         * before the "source" field existed (Settings' Save button), so
         * any profile.json that exists on disk and predates "source" was
         * necessarily written by a human, not left over from defaults.
         * Without this, upgrading would suddenly treat someone's
         * already-configured printer as unconfigured and lock the
         * control panel until they re-save. Backfilled once here and
         * re-saved so every later load reads it directly. */
        if (!cJSON_HasObjectItem(json, "source") &&
            profile->source == PRINTER_PROFILE_SOURCE_DEFAULT) {
            profile->source = PRINTER_PROFILE_SOURCE_MANUAL;
            printer_profile_save(profile, path);
        }

        cJSON_Delete(json);
    }
    /* If parsing failed, we just keep the defaults set above — a
     * corrupted profile file shouldn't stop the backend from starting. */
}

/* Makes sure the directory containing `path` exists, creating it if
 * needed. Only handles a single-level directory (e.g. "data" in
 * "data/profile.json") since that's all we need here — not a general
 * "mkdir -p" implementation. */
static void ensure_parent_directory_exists(const char *path) {
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *last_slash = strrchr(dir, '/');
    if (last_slash == NULL) {
        return; /* no directory component, nothing to create */
    }
    *last_slash = '\0';

    /* mkdir() fails with EEXIST if the directory is already there, which
     * is fine and expected on every run after the first — we don't need
     * to check the error, just attempt it. Permissions 0755 = owner can
     * read/write/list, everyone else can read/list. */
    mkdir(dir, 0755);
}

int printer_profile_save(const PrinterProfile *profile, const char *path) {
    ensure_parent_directory_exists(path);

    cJSON *json = printer_profile_to_json(profile);
    char *text = cJSON_Print(json);
    cJSON_Delete(json);

    if (text == NULL) {
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(text);
        return -1;
    }

    fputs(text, file);
    fclose(file);
    free(text);

    return 0;
}
