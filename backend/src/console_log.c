/*
 * console_log.c
 * ==============
 * See console_log.h for the overview. This file owns both the ring
 * buffer storage and its REST + WebSocket routes together (unlike most
 * routes, which live in api_handlers.c) — GET /api/console is really
 * just "dump the ring buffer as JSON", tightly coupled to this struct's
 * internal layout, so keeping it next to that layout felt more cohesive
 * than splitting it across two files.
 */

/* clock_gettime() needs this under -std=c99 — see the identical comment
 * in transport_serial.c. */
#define _DEFAULT_SOURCE

#include "console_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "civetweb.h"

static long long current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *direction_to_string(ConsoleDirection direction) {
    return (direction == CONSOLE_DIRECTION_SENT) ? "sent" : "received";
}

static cJSON *entry_to_json(const ConsoleEntry *entry) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "direction", direction_to_string(entry->direction));
    cJSON_AddStringToObject(json, "text", entry->text);
    cJSON_AddNumberToObject(json, "timestampMs", (double)entry->timestamp_ms);
    return json;
}

void console_log_init(ConsoleLog *log) {
    memset(log, 0, sizeof(*log));
    pthread_mutex_init(&log->lock, NULL);
}

/* Pushes `entry` to every connected WebSocket client, dropping any whose
 * write fails (same "compact the array" approach as
 * ws_broadcaster_send_state — see that function's comment for why). Must
 * be called with log->lock already held, since it reads/writes
 * ws_clients directly. */
static void broadcast_entry_locked(ConsoleLog *log, const ConsoleEntry *entry) {
    cJSON *json = entry_to_json(entry);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        return;
    }

    int write_index = 0;
    for (int i = 0; i < log->ws_client_count; i++) {
        struct mg_connection *conn = log->ws_clients[i];
        int result = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text, strlen(text));
        if (result > 0) {
            log->ws_clients[write_index] = conn;
            write_index++;
        }
    }
    log->ws_client_count = write_index;

    free(text);
}

void console_log_append(ConsoleLog *log, ConsoleDirection direction, const char *text) {
    pthread_mutex_lock(&log->lock);

    ConsoleEntry *slot = &log->entries[log->next_slot];
    slot->direction = direction;
    strncpy(slot->text, text, sizeof(slot->text) - 1);
    slot->text[sizeof(slot->text) - 1] = '\0';
    slot->timestamp_ms = current_time_ms();

    log->next_slot = (log->next_slot + 1) % CONSOLE_LOG_CAPACITY;
    if (log->count < CONSOLE_LOG_CAPACITY) {
        log->count++;
    }

    broadcast_entry_locked(log, slot);

    pthread_mutex_unlock(&log->lock);
}

/* ---------------------------------------------------------------------
 * GET /api/console
 * --------------------------------------------------------------------- */

static int console_get_handler(struct mg_connection *conn, void *cbdata) {
    ConsoleLog *log = (ConsoleLog *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    pthread_mutex_lock(&log->lock);

    /* The ring buffer's oldest entry is at next_slot once it's full
     * (that's the slot about to be overwritten next); before it's full,
     * the oldest entry is simply index 0 and next_slot is just the next
     * empty slot. Either way, walking `count` entries starting at the
     * right place and wrapping with modulo visits them oldest-first. */
    int start = (log->count < CONSOLE_LOG_CAPACITY) ? 0 : log->next_slot;

    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < log->count; i++) {
        int index = (start + i) % CONSOLE_LOG_CAPACITY;
        cJSON_AddItemToArray(array, entry_to_json(&log->entries[index]));
    }

    pthread_mutex_unlock(&log->lock);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "entries", array);

    char *text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (text == NULL) {
        mg_send_http_error(conn, 500, "Failed to build response");
        return 1;
    }

    mg_send_http_ok(conn, "application/json", (long long)strlen(text));
    mg_write(conn, text, strlen(text));
    free(text);

    return 1;
}

/* ---------------------------------------------------------------------
 * /api/ws/console — same connect/ready/data/close shape as
 * ws_broadcaster.c, see that file for the detailed reasoning behind each
 * callback's behavior.
 * --------------------------------------------------------------------- */

static int console_ws_connect_handler(const struct mg_connection *conn, void *cbdata) {
    (void)conn;
    (void)cbdata;
    return 0;
}

static void console_ws_ready_handler(struct mg_connection *conn, void *cbdata) {
    ConsoleLog *log = (ConsoleLog *)cbdata;

    pthread_mutex_lock(&log->lock);
    if (log->ws_client_count < CONSOLE_LOG_MAX_WS_CLIENTS) {
        log->ws_clients[log->ws_client_count] = conn;
        log->ws_client_count++;
    }
    pthread_mutex_unlock(&log->lock);
}

static int console_ws_data_handler(struct mg_connection *conn, int bits, char *data,
                                   size_t data_len, void *cbdata) {
    (void)conn;
    (void)data;
    (void)data_len;
    (void)cbdata;

    int opcode = bits & 0x0F;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
        return 0;
    }
    return 1;
}

static void console_ws_close_handler(const struct mg_connection *conn, void *cbdata) {
    ConsoleLog *log = (ConsoleLog *)cbdata;

    pthread_mutex_lock(&log->lock);
    for (int i = 0; i < log->ws_client_count; i++) {
        if (log->ws_clients[i] == conn) {
            for (int j = i; j < log->ws_client_count - 1; j++) {
                log->ws_clients[j] = log->ws_clients[j + 1];
            }
            log->ws_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&log->lock);
}

void console_log_register_routes(struct mg_context *ctx, ConsoleLog *log) {
    mg_set_request_handler(ctx, "/api/console", console_get_handler, log);
    mg_set_websocket_handler(ctx, "/api/ws/console", console_ws_connect_handler,
                             console_ws_ready_handler, console_ws_data_handler,
                             console_ws_close_handler, log);
}
