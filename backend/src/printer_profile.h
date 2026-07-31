#ifndef REMOTICA_PRINTER_PROFILE_H
#define REMOTICA_PRINTER_PROFILE_H

/*
 * printer_profile.h
 * ==================
 * The "printer profile" is the set of physical facts about a specific
 * printer that don't change while it's running: how big the bed is, how
 * tall the gantry can go, what temperature it needs before it'll extrude.
 *
 * Right now the frontend hardcodes these in frontend/src/lib/
 * printer-profile.js, with a comment saying "placeholder until a real
 * printer-profile/Settings feature exists". This module is that real
 * feature's backend half: it stores the profile in a small JSON file on
 * disk (backend/data/profile.json) so it survives a restart, and the
 * frontend's future Settings page can read/edit it over the REST API
 * (see api_handlers.c's GET/POST /api/profile).
 */

struct cJSON;

typedef struct {
    double bed_width_mm;
    double bed_depth_mm;
    double max_z_mm;
    double min_extrude_temp_c;
    double max_hotend_temp_c;
    double max_bed_temp_c;
} PrinterProfile;

/* Fills in the same numbers the frontend currently hardcodes, so the
 * backend and frontend start out in agreement even before a profile file
 * exists on disk. */
void printer_profile_defaults(PrinterProfile *profile);

/* Loads the profile from a JSON file at `path`. If the file doesn't exist
 * yet (e.g. first run) or fails to parse, this falls back to
 * printer_profile_defaults() instead of erroring out — a missing profile
 * file just means "nobody has customized it yet", not a real failure. */
void printer_profile_load(PrinterProfile *profile, const char *path);

/* Writes the profile out to `path` as JSON. Creates the parent directory
 * if it doesn't exist yet. Returns 0 on success, -1 on failure (e.g. the
 * directory couldn't be created, or the file couldn't be written). */
int printer_profile_save(const PrinterProfile *profile, const char *path);

/* Converts to/from cJSON, used both for talking to the frontend over HTTP
 * and for the on-disk file format (they're the same shape on purpose —
 * one less thing to keep in sync). from_json only overwrites fields that
 * are actually present in the given JSON object, so a partial update like
 * {"maxZMm": 300} leaves the other fields untouched. Returns 0 on success. */
struct cJSON *printer_profile_to_json(const PrinterProfile *profile);
int printer_profile_from_json(PrinterProfile *profile, const struct cJSON *json);

#endif /* REMOTICA_PRINTER_PROFILE_H */
