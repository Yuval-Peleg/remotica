/*
 * printer_database.c
 * ====================
 * See printer_database.h for the "why". This file is just the "what": the
 * actual table of printers and specs, plus the JSON conversion.
 */

#include "printer_database.h"

#include "cJSON.h"

/* clang-format off */
static const PrinterDatabaseEntry k_printers[] = {
    {"ender3", "Creality Ender 3 / Ender 3 Pro",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"ender3v2", "Creality Ender 3 V2",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"ender3s1", "Creality Ender 3 S1",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 270,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"ender5", "Creality Ender 5",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 300,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"cr10", "Creality CR-10",
     {.bed_width_mm = 300, .bed_depth_mm = 300, .max_z_mm = 400,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"prusamk3s", "Prusa i3 MK3S+",
     {.bed_width_mm = 250, .bed_depth_mm = 210, .max_z_mm = 210,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 120}},
    {"prusamk4", "Prusa MK4",
     {.bed_width_mm = 250, .bed_depth_mm = 210, .max_z_mm = 220,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 120}},
    {"prusamini", "Prusa Mini+",
     {.bed_width_mm = 180, .bed_depth_mm = 180, .max_z_mm = 180,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 300, .max_bed_temp_c = 100}},
    {"anycubickobra2", "Anycubic Kobra 2",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 250,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"anycubicvyper", "Anycubic Vyper",
     {.bed_width_mm = 245, .bed_depth_mm = 245, .max_z_mm = 260,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
    {"artillerysidewinderx2", "Artillery Sidewinder X2",
     {.bed_width_mm = 300, .bed_depth_mm = 300, .max_z_mm = 400,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 240, .max_bed_temp_c = 130}},
    {"elegoo-neptune3", "Elegoo Neptune 3",
     {.bed_width_mm = 220, .bed_depth_mm = 220, .max_z_mm = 280,
      .min_extrude_temp_c = 170, .max_hotend_temp_c = 260, .max_bed_temp_c = 100}},
};
/* clang-format on */

#define PRINTER_COUNT ((int)(sizeof(k_printers) / sizeof(k_printers[0])))

const PrinterDatabaseEntry *printer_database_get(int *out_count) {
    *out_count = PRINTER_COUNT;
    return k_printers;
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
