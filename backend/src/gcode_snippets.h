#ifndef REMOTICA_GCODE_SNIPPETS_H
#define REMOTICA_GCODE_SNIPPETS_H

/*
 * gcode_snippets.h
 * =================
 * Two blocks of user-written G-code that run around every print: one
 * before the sliced file, one after it. The equivalent of OctoPrint's
 * start/end GCODE scripts — a purge line, a nozzle wipe, parking the
 * head, presenting the bed.
 *
 * *** Deliberately NOT part of PrinterProfile. ***
 * That struct's `source` field is what unlocks the printer's controls
 * (see profile_is_configured() in api_handlers.c), and POST /api/profile
 * sets it to MANUAL. If snippets lived there, two things would follow:
 * saving a macro would go through that endpoint and so silently count as
 * confirming the printer — defeating the confirmation gate through a
 * side door — and applying a preset, which replaces the profile's
 * contents, would wipe the user's snippets because presets carry no
 * G-code. Both are fixable with merge rules; merge rules are what gets
 * forgotten later. Separate file, separate endpoint, no interaction.
 */

#include <stddef.h>

struct cJSON;

/* Per snippet. Generous for the handful of lines these hold in practice,
 * small enough that two of them plus overhead stay well inside the JSON
 * body limit in api_handlers.c. */
#define GCODE_SNIPPET_MAX 2048

typedef struct {
    char start_gcode[GCODE_SNIPPET_MAX];
    char end_gcode[GCODE_SNIPPET_MAX];

    /* The profile's printerName at the moment these were last saved.
     * Exists only so Settings can warn — blockingly — when the user
     * switches to a different printer while snippets are set: start
     * G-code that homes or heats correctly for one machine can be wrong
     * on another. Nothing in the backend reads it. */
    char written_for[64];
} GcodeSnippets;

/* Empty snippets — the state where this feature does nothing at all. */
void gcode_snippets_defaults(GcodeSnippets *snippets);

/* Reads `path`, falling back to defaults if it's missing (first run) or
 * unparseable. A corrupt snippets file must never stop the backend
 * starting: the printer stays usable, the macros just don't run. */
void gcode_snippets_load(GcodeSnippets *snippets, const char *path);

/* Writes `path`. Returns 0 on success, -1 on failure. */
int gcode_snippets_save(const GcodeSnippets *snippets, const char *path);

/* Caller owns the returned object. */
struct cJSON *gcode_snippets_to_json(const GcodeSnippets *snippets);

/* Overwrites only the fields actually present and of the right type, so a
 * partial body can't silently blank the others. */
void gcode_snippets_from_json(GcodeSnippets *snippets, const struct cJSON *json);

#endif /* REMOTICA_GCODE_SNIPPETS_H */
