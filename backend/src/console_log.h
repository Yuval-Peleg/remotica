#ifndef REMOTICA_CONSOLE_LOG_H
#define REMOTICA_CONSOLE_LOG_H

/*
 * console_log.h
 * ==============
 * A live transcript of the actual gcode conversation between this
 * backend and the printer — every line sent, and every line received
 * back — so the frontend can show a "terminal" view of what's really
 * happening on the wire. This is purely observational: nothing in this
 * file sends commands or changes printer behavior, it just records what
 * the drivers (transport_sim.c / transport_serial.c) are already doing.
 *
 * Two ways to read it, matching the same REST+WebSocket split used for
 * printer_state.h:
 *   - GET /api/console: the recent backlog, so opening the console for
 *     the first time isn't empty until something new happens to log.
 *   - /api/ws/console: pushes each new entry the moment it's appended
 *     (not on a fixed tick like the state broadcaster — a terminal
 *     should feel instant, not delayed up to 300ms).
 *
 * Kept as its own small module (rather than folding into
 * ws_broadcaster.h or printer_state.h) because it's a genuinely
 * different concern: an append-only history log with its own client
 * list, versus "the current truth" that printer_state.h tracks.
 */

#include <pthread.h>

struct mg_context;
struct mg_connection;

typedef enum {
    CONSOLE_DIRECTION_SENT,    /* a line this backend sent to the printer */
    CONSOLE_DIRECTION_RECEIVED /* a line the printer sent back */
} ConsoleDirection;

/* Generous enough for any line this project actually sends/receives
 * (gcode commands and their "ok"/temperature-report replies are all
 * well under 100 characters in practice). */
#define CONSOLE_LOG_TEXT_MAX 200

/* How many recent lines are kept in memory for the backlog / GET
 * /api/console. Once full, the oldest entry is overwritten (a ring
 * buffer) — this is a live debugging aid, not a permanent print log, so
 * there's no need to ever persist it to disk. */
#define CONSOLE_LOG_CAPACITY 300

#define CONSOLE_LOG_MAX_WS_CLIENTS 16

typedef struct {
    ConsoleDirection direction;
    char text[CONSOLE_LOG_TEXT_MAX];
    long long timestamp_ms; /* milliseconds since epoch */
} ConsoleEntry;

/* Named struct tag (not an anonymous one) on purpose: other files
 * (transport.h, transport_serial.h) need to forward-declare this as
 * `struct ConsoleLog;` without including this whole header, and that
 * only works if the tag name matches the typedef name — see cJSON.h's
 * `typedef struct cJSON { ... } cJSON;` for the same pattern. */
typedef struct ConsoleLog {
    pthread_mutex_t lock;

    ConsoleEntry entries[CONSOLE_LOG_CAPACITY];
    int count;     /* how many slots are filled so far, caps at CONSOLE_LOG_CAPACITY */
    int next_slot; /* ring buffer write cursor */

    /* Same "track connections, broadcast, compact on failed write"
     * pattern as WsBroadcaster in ws_broadcaster.h — see that file's
     * comments for why it looks the way it does. Kept separate from
     * WsBroadcaster because these are two different WebSocket endpoints
     * with two different sets of subscribers (not everyone watching
     * live temps cares about the raw gcode feed, and vice versa). */
    struct mg_connection *ws_clients[CONSOLE_LOG_MAX_WS_CLIENTS];
    int ws_client_count;
} ConsoleLog;

void console_log_init(ConsoleLog *log);

/* Records one line and immediately pushes it to every connected
 * /api/ws/console client. Safe to call from any thread. `text` is
 * copied (truncated if longer than CONSOLE_LOG_TEXT_MAX - 1), so the
 * caller doesn't need to keep it alive afterward. */
void console_log_append(ConsoleLog *log, ConsoleDirection direction, const char *text);

/* Registers GET /api/console and the /api/ws/console WebSocket endpoint
 * on ctx. */
void console_log_register_routes(struct mg_context *ctx, ConsoleLog *log);

#endif /* REMOTICA_CONSOLE_LOG_H */
