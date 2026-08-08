#ifndef REMOTICA_API_HANDLERS_H
#define REMOTICA_API_HANDLERS_H

/*
 * api_handlers.h
 * ===============
 * The REST API: the "menu" of one-off requests the frontend can make
 * (see CLAUDE.md's "Planned backend" section — this is that plan, built).
 *
 * civetweb calls a registered handler function for each matching request,
 * and passes back whatever "cbdata" pointer we gave it at registration
 * time (see mg_set_request_handler in main.c). We use that to hand every
 * handler an AppContext — the small bundle of shared pointers (printer
 * state, the active driver, the profile, file paths) it needs to actually
 * do anything. This avoids using global variables to share this data
 * between files, which keeps it obvious exactly what each handler depends
 * on just by looking at its signature.
 */

#include <time.h>

#include "gcode_snippets.h"
#include "job_manager.h"
#include "printer_profile.h"
#include "printer_state.h"
#include "transport.h"

typedef struct {
    PrinterState *state;
    PrinterDriver *driver;
    PrinterProfile *profile;

    const char *profile_path; /* where printer_profile_save() writes to */
    const char *uploads_dir;  /* where uploaded gcode files are stored */

    /* Reported by GET /api/system. None of these affect printer
     * behaviour — they exist so the System page can answer "what is this
     * machine running, and will it come back after a power cut?". */
    const char *web_root;      /* NULL when not serving a built frontend */
    const char *data_dir;      /* what free space is reported against */
    const char *serial_device; /* NULL means the simulator */
    time_t started_at;         /* for uptime */

    /* Start/end G-code that runs around a print. Kept out of
     * PrinterProfile on purpose — see gcode_snippets.h. */
    GcodeSnippets *snippets;
    const char *snippets_path;
} AppContext;

struct mg_context;

/* Registers every REST route this backend understands onto civetweb's
 * `ctx`, using `app_context` as the shared data every handler receives.
 * Called once from main.c after both civetweb and the AppContext are set
 * up. `app_context` must stay valid for as long as the server runs (main
 * keeps it on the stack for its whole lifetime, so this is safe). */
void api_handlers_register_all(struct mg_context *ctx, AppContext *app_context);

#endif /* REMOTICA_API_HANDLERS_H */
