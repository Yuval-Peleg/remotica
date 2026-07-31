/*
 * ws_broadcaster.c
 * =================
 * See ws_broadcaster.h for the overview. This file implements the four
 * callbacks civetweb calls during a WebSocket connection's lifetime
 * (connect, ready, data, close), plus the actual broadcast function.
 */

#include "ws_broadcaster.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "civetweb.h"

void ws_broadcaster_init(WsBroadcaster *broadcaster) {
    pthread_mutex_init(&broadcaster->lock, NULL);
    broadcaster->client_count = 0;
}

/* Called before the WebSocket handshake completes. We don't have any
 * reason to reject a connection here (no auth yet, no per-client state to
 * set up), so this always accepts. Returning 0 means "go ahead and
 * complete the handshake" (see the mg_websocket_connect_handler doc
 * comment in civetweb.h — 0 = proceed, 1 = reject). */
static int ws_connect_handler(const struct mg_connection *conn, void *cbdata) {
    (void)conn;
    (void)cbdata;
    return 0;
}

/* Called once the handshake is done and the connection is ready for
 * data. This is where we start tracking the connection so future
 * broadcasts reach it. */
static void ws_ready_handler(struct mg_connection *conn, void *cbdata) {
    WsBroadcaster *broadcaster = (WsBroadcaster *)cbdata;

    pthread_mutex_lock(&broadcaster->lock);
    if (broadcaster->client_count < WS_BROADCASTER_MAX_CLIENTS) {
        broadcaster->clients[broadcaster->client_count] = conn;
        broadcaster->client_count++;
    }
    /* If we're already at WS_BROADCASTER_MAX_CLIENTS, we simply don't
     * track this connection — it stays open, but won't receive
     * broadcasts. For a small LAN tool this ceiling is generous enough
     * that hitting it would already indicate something unusual going on
     * (e.g. a client endlessly reconnecting without cleaning up). */
    pthread_mutex_unlock(&broadcaster->lock);
}

/* Called whenever the client sends us a WebSocket frame. This backend's
 * API design (see CLAUDE.md) uses this connection purely as a one-way
 * push channel — the frontend sends all its actions over REST instead
 * (jog, temps, etc.) — so there's normally nothing meaningful for the
 * client to send us here. We still have to handle the frame politely:
 * the low 4 bits of `bits` are the WebSocket opcode (RFC 6455 section
 * 5.2 — the upper bits are flags like FIN, not part of the opcode), and
 * if the client is telling us it's closing the connection, we should
 * agree by returning 0 rather than pretending everything's still open. */
static int ws_data_handler(struct mg_connection *conn, int bits, char *data, size_t data_len,
                           void *cbdata) {
    (void)conn;
    (void)data;
    (void)data_len;
    (void)cbdata;

    int opcode = bits & 0x0F;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
        return 0; /* agree to close */
    }

    return 1; /* keep the connection open; we ignore whatever was sent */
}

/* Called once the connection is actually closed (client navigated away,
 * lost network, etc). Stop tracking it so we don't try to write to a
 * dead connection on the next broadcast. */
static void ws_close_handler(const struct mg_connection *conn, void *cbdata) {
    WsBroadcaster *broadcaster = (WsBroadcaster *)cbdata;

    pthread_mutex_lock(&broadcaster->lock);
    for (int i = 0; i < broadcaster->client_count; i++) {
        if (broadcaster->clients[i] == conn) {
            /* Shift everything after this one down by one slot, then
             * shrink the count — keeps the array packed with no gaps,
             * which is what lets ws_broadcaster_send_state() below just
             * loop from 0 to client_count without needing to skip empty
             * slots. */
            for (int j = i; j < broadcaster->client_count - 1; j++) {
                broadcaster->clients[j] = broadcaster->clients[j + 1];
            }
            broadcaster->client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&broadcaster->lock);
}

void ws_broadcaster_register(struct mg_context *ctx, WsBroadcaster *broadcaster) {
    mg_set_websocket_handler(ctx, "/api/ws", ws_connect_handler, ws_ready_handler, ws_data_handler,
                             ws_close_handler, broadcaster);
}

void ws_broadcaster_send_state(WsBroadcaster *broadcaster, PrinterState *state) {
    cJSON *json = printer_state_to_json(state);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (text == NULL) {
        return;
    }

    pthread_mutex_lock(&broadcaster->lock);

    /* Write to every tracked client, keeping only the ones the write
     * actually succeeded for. mg_websocket_write returns <= 0 if the
     * connection has already dropped — rather than leaving dead
     * connections in the list until their close_handler eventually fires
     * (which might be delayed), we drop them here immediately by
     * "compacting" the array: write_index tracks where the next
     * still-good connection should go, and by the end anything at index
     * >= write_index is a stale entry we simply overwrite / forget by
     * shrinking client_count down to write_index. */
    int write_index = 0;
    for (int i = 0; i < broadcaster->client_count; i++) {
        struct mg_connection *conn = broadcaster->clients[i];
        int result = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text, strlen(text));
        if (result > 0) {
            broadcaster->clients[write_index] = conn;
            write_index++;
        }
    }
    broadcaster->client_count = write_index;

    pthread_mutex_unlock(&broadcaster->lock);

    free(text);
}
