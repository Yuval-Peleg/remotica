#ifndef REMOTICA_PRINTER_DATABASE_H
#define REMOTICA_PRINTER_DATABASE_H

/*
 * printer_database.h
 * ====================
 * A small, hand-curated table of common consumer 3D printers' published
 * physical specs (bed size, max Z, safe temperature limits) — NOT
 * detected from anything, just a quick-fill list the frontend's Settings
 * page can offer in a "select your printer" dropdown, so a user doesn't
 * have to look their printer's bed size up by hand.
 *
 * This is hand-curated rather than vendored from an existing third-party
 * database on purpose: Cura's and PrusaSlicer's bundled printer
 * definitions would be the obvious source, but they're LGPLv3 and AGPLv3
 * respectively, which doesn't mix cleanly with this MIT-licensed repo.
 * The numbers here (bed size, max Z, temp limits) are each printer's own
 * published specs — facts, not copyrightable — cross-checked against
 * their manufacturer documentation rather than copied from those repos.
 *
 * Deliberately NOT tied to auto-detection: a USB serial connection can't
 * reliably identify which specific printer model is on the other end
 * (see transport_serial.c's M115 handling — that's logged as a
 * best-effort hint at most, never trusted as proof), so this list exists
 * purely for a human to pick from.
 */

#include "printer_profile.h"

typedef struct {
    const char *id;   /* short stable identifier, e.g. "ender3" */
    const char *name; /* display name, e.g. "Creality Ender 3" */
    PrinterProfile profile;
    /* The exact MACHINE_TYPE value this printer's own firmware has been
     * confirmed (against its official firmware source, or real hardware)
     * to report over M115 - e.g. "Ender-3". NULL for every entry we
     * haven't verified, which is most of them: many printers' firmware
     * doesn't set this at all, and guessing risks silently applying a
     * *different* printer's temp/bed limits, which is worse than no
     * auto-detect. Only add a value here once it's actually confirmed -
     * see printer_database.c's k_printers table for what's verified so
     * far and why the rest deliberately aren't. */
    const char *verified_machine_type;
} PrinterDatabaseEntry;

/* Returns a pointer to the static, read-only list of known printers, and
 * writes how many entries it has into *out_count. The caller does not
 * own this memory and must not modify or free it. */
const PrinterDatabaseEntry *printer_database_get(int *out_count);

/* Given a raw firmware_info string (e.g. state->firmware_info), returns a
 * pointer to the one PrinterDatabaseEntry whose verified_machine_type
 * exactly matches (case/dash/underscore-insensitive) the string's
 * MACHINE_TYPE:<value> token, or NULL if there's no match - including
 * when firmware_info has no MACHINE_TYPE token at all, or the token
 * doesn't equal any verified entry. Deliberately exact-equality only,
 * never a substring/fuzzy match: "Ender-3" and "Ender-3 V2" must never be
 * treated as the same printer just because one contains the other. */
const PrinterDatabaseEntry *printer_database_match_firmware(const char *firmware_info);

struct cJSON;

/* Builds a fresh cJSON array with one object per entry — the same field
 * shape printer_profile_to_json() produces, plus "id" and "name" — for
 * GET /api/printer-database. Caller owns the result and must
 * cJSON_Delete() it. */
struct cJSON *printer_database_to_json(void);

#endif /* REMOTICA_PRINTER_DATABASE_H */
