#ifndef REMOTICA_WS_BROADCASTER_H
#define REMOTICA_WS_BROADCASTER_H

/*
 * ws_broadcaster.h
 * =================
 * The WebSocket half of the REST+WebSocket hybrid API described in
 * CLAUDE.md: REST handles one-off actions (see api_handlers.h), and this
 * handles the "continuously push live state to every connected browser"
 * part, so the frontend doesn't have to keep asking "has anything
 * changed yet?" (that's what polling would look like, and it's both
 * wasteful and laggy compared to the server just telling you the moment
 * something changes).
 *
 * A WebSocket connection, once established, stays open — unlike a normal
 * HTTP request/response which is done after one reply. That means we
 * need to keep track of every browser that's currently connected, so
 * that when it's time to broadcast a state update we know who to send it
 * to. That bookkeeping (a small list of active connections, protected by
 * a mutex since browsers can connect/disconnect from civetweb's worker
 * threads at any time) is what WsBroadcaster is for.
 */

#include <pthread.h>

#include "printer_state.h"

struct mg_context;
struct mg_connection;

/* How many browsers can be connected to the live-state WebSocket at
 * once. This is a small hobby/LAN project — nobody is going to have
 * hundreds of tabs open — so a small fixed-size list is simpler and
 * plenty, instead of a dynamically-growing one. */
#define WS_BROADCASTER_MAX_CLIENTS 16

typedef struct {
    pthread_mutex_t lock;
    struct mg_connection *clients[WS_BROADCASTER_MAX_CLIENTS];
    int client_count;
} WsBroadcaster;

/* Initializes the broadcaster (empty client list, ready mutex). Call
 * once before registering it with civetweb. */
void ws_broadcaster_init(WsBroadcaster *broadcaster);

/* Registers the WebSocket endpoint (at "/api/ws") on `ctx`, wiring up
 * civetweb's connect/ready/data/close callbacks to this broadcaster. */
void ws_broadcaster_register(struct mg_context *ctx, WsBroadcaster *broadcaster);

/* Builds a JSON snapshot of `state` (same shape as GET /api/state — see
 * printer_state_to_json) and sends it to every currently-connected
 * client. Called from main.c's background tick loop, so the frontend
 * gets fresh data automatically without asking for it. Safe to call even
 * with zero connected clients (it just does nothing). */
void ws_broadcaster_send_state(WsBroadcaster *broadcaster, PrinterState *state);

#endif /* REMOTICA_WS_BROADCASTER_H */
