/*
 * printer_database.c
 * ====================
 * See printer_database.h for the "why". This file is just the "what": the
 * actual table of printers and specs, plus the JSON conversion.
 */

#include "printer_database.h"

#include <ctype.h>
#include <string.h>

#include "cJSON.h"

/* clang-format off */
static const PrinterDatabaseEntry k_printers[] = {
    {"ender3", "Creality Ender 3 / Ender 3 Pro",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = "Ender-3"},
    {"ender3v2", "Creality Ender 3 V2",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = "Ender-3 V2"},
    {"ender3s1", "Creality Ender 3 S1",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 270,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = "Ender-3 S1"},
    /* No verified_machine_type below this line: either no official
     * firmware source could be found (ender5, elegoo-neptune3), the
     * official source exists but never sets a custom machine name at all
     * (cr10), Prusa's firmware doesn't use this MACHINE_TYPE mechanism
     * the way stock Marlin does (prusa*), or — anycubickobra2 and
     * anycubicvyper specifically — their official firmware repos were
     * both found to report the exact same string ("Anycubic Viper"),
     * which would make the two printers indistinguishable by this field
     * alone. Matching either one would risk silently mislabeling it as
     * the other, so neither is matched. See root CLAUDE.md/the commit
     * that added this for the research behind each of these. */
    {"ender5", "Creality Ender 5",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 300,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
    {"cr10", "Creality CR-10",
     {.bed_width_mm = 300, .bed_depth_mm = 300, .max_z_mm = 400,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
    {"prusamk3s", "Prusa i3 MK3S+",
     {.bed_width_mm = 250, .bed_depth_mm = 210, .max_z_mm = 210,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 120},
     .verified_machine_type = NULL},
    {"prusamk4", "Prusa MK4",
     {.bed_width_mm = 250, .bed_depth_mm = 210, .max_z_mm = 220,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 120},
     .verified_machine_type = NULL},
    {"prusamini", "Prusa Mini+",
     {.bed_width_mm = 180, .bed_depth_mm = 180, .max_z_mm = 180,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
    {"anycubickobra2", "Anycubic Kobra 2",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
    {"anycubicvyper", "Anycubic Vyper",
     {.bed_width_mm = 245, .bed_depth_mm = 245, .max_z_mm = 260,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
    {"artillerysidewinderx2", "Artillery Sidewinder X2",
     {.bed_width_mm = 300, .bed_depth_mm = 300, .max_z_mm = 400,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 240, .max_bed_temp_c = 130},
     .verified_machine_type = "Artillery Sidewinder X2"},
    {"elegoo-neptune3", "Elegoo Neptune 3",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 280,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100},
     .verified_machine_type = NULL},
};
/* clang-format on */

#define PRINTER_COUNT ((int)(sizeof(k_printers) / sizeof(k_printers[0])))

const PrinterDatabaseEntry *printer_database_get(int *out_count) {
    *out_count = PRINTER_COUNT;
    return k_printers;
}

/* True if `p` looks like the start of the NEXT M115 field — an
 * uppercase/underscore/digit run immediately followed by ':', e.g.
 * "KINEMATICS:" or "EXTRUDER_COUNT:" or "UUID:". Used below to find
 * where a MACHINE_TYPE value actually ends. */
static int looks_like_field_start(const char *p) {
    if (!(isupper((unsigned char)*p) || *p == '_')) {
        return 0;
    }
    while (*p != '\0' && *p != ' ') {
        if (*p == ':') {
            return 1;
        }
        if (!(isupper((unsigned char)*p) || *p == '_' || isdigit((unsigned char)*p))) {
            return 0;
        }
        p++;
    }
    return 0;
}

/* Pulls the MACHINE_TYPE:<value> token out of a raw M115 reply — the C
 * equivalent of frontend/src/lib/parse-printer-name.js's regex
 * (/\bMACHINE_TYPE:(\S+)/), reimplemented here since this is C, not JS,
 * with one deliberate difference: this stops at the next FIELD, not the
 * next whitespace. A firmware's CUSTOM_MACHINE_NAME can itself contain a
 * space (e.g. Creality's official "Ender-3 V2") — M115 has no quoting,
 * and Marlin's own M115.cpp emits it as raw concatenation
 * (`" MACHINE_TYPE:" MACHINE_NAME " KINEMATICS:" ...`), so a naive
 * first-whitespace cut would truncate "Ender-3 V2" down to just
 * "Ender-3" and silently match the wrong printer. Confirmed against
 * Marlin's real M115.cpp source before relying on this. Returns 1 and
 * writes the token into *out on success, 0 if no MACHINE_TYPE: field was
 * found. */
static int extract_machine_type_token(const char *firmware_info, char *out, size_t out_size) {
    const char *key = "MACHINE_TYPE:";
    const char *pos = strstr(firmware_info, key);
    if (pos == NULL) {
        return 0;
    }
    pos += strlen(key);

    size_t i = 0;
    while (i < out_size - 1 && pos[i] != '\0') {
        if (isspace((unsigned char)pos[i]) && looks_like_field_start(pos + i + 1)) {
            break;
        }
        out[i] = pos[i];
        i++;
    }
    /* Trim any trailing whitespace (e.g. the value is genuinely the last
     * field on the line, with only a trailing \r\n after it). */
    while (i > 0 && isspace((unsigned char)out[i - 1])) {
        i--;
    }
    out[i] = '\0';
    return i > 0;
}

/* Normalizes a MACHINE_TYPE token for comparison: lowercases every
 * alphanumeric character and drops everything else ('-', '_', spaces),
 * so "Ender-3", "ender_3", and "ENDER3" all compare equal. Still an
 * EXACT-equality building block, not a substring one: "Ender-3" and
 * "Ender-3 V2" normalize to "ender3" and "ender3v2" respectively —
 * different strings, so one can never accidentally match the other. */
static void normalize_machine_type(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j < out_size - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
        }
    }
    out[j] = '\0';
}

const PrinterDatabaseEntry *printer_database_match_firmware(const char *firmware_info) {
    if (firmware_info == NULL || firmware_info[0] == '\0') {
        return NULL;
    }

    char token[64];
    if (!extract_machine_type_token(firmware_info, token, sizeof(token))) {
        return NULL;
    }

    char normalized_token[64];
    normalize_machine_type(token, normalized_token, sizeof(normalized_token));

    int count;
    const PrinterDatabaseEntry *entries = printer_database_get(&count);
    for (int i = 0; i < count; i++) {
        if (entries[i].verified_machine_type == NULL) {
            continue; /* unverified entry — never auto-matched, see k_printers's comment */
        }
        char normalized_entry[64];
        normalize_machine_type(entries[i].verified_machine_type, normalized_entry,
                               sizeof(normalized_entry));
        if (strcmp(normalized_token, normalized_entry) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

cJSON *printer_database_to_json(void) {
    cJSON *array = cJSON_CreateArray();

    for (int i = 0; i < PRINTER_COUNT; i++) {
        const PrinterDatabaseEntry *entry = &k_printers[i];
        /* Reuses the exact same field shape GET /api/profile already
         * returns, so the frontend can apply a database entry to its
         * profile form with no special-casing — just id/name plus
         * whatever printer_profile_to_json() already produces. */
        cJSON *item = printer_profile_to_json(&entry->profile);
        cJSON_AddStringToObject(item, "id", entry->id);
        cJSON_AddStringToObject(item, "name", entry->name);
        cJSON_AddItemToArray(array, item);
    }

    return array;
}
