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
} PrinterDatabaseEntry;

/* Returns a pointer to the static, read-only list of known printers, and
 * writes how many entries it has into *out_count. The caller does not
 * own this memory and must not modify or free it. */
const PrinterDatabaseEntry *printer_database_get(int *out_count);

struct cJSON;

/* Builds a fresh cJSON array with one object per entry — the same field
 * shape printer_profile_to_json() produces, plus "id" and "name" — for
 * GET /api/printer-database. Caller owns the result and must
 * cJSON_Delete() it. */
struct cJSON *printer_database_to_json(void);

#endif /* REMOTICA_PRINTER_DATABASE_H */
