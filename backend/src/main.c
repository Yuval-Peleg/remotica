#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "civetweb.h"

#define LISTEN_PORT "8080"
#define MAX_BODY_BYTES 65536

static volatile sig_atomic_t s_stop_requested = 0;

static void handle_stop_signal(int sig_num) {
    (void)sig_num;
    s_stop_requested = 1;
}

/* POST /api/command — takes any JSON body, prints it, and echoes it back.
 * This is a connectivity smoke test: it proves the frontend can reach the
 * backend, not a real command yet. */
static int command_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;

    const struct mg_request_info *req_info = mg_get_request_info(conn);
    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Only POST is supported here");
        return 1;
    }

    char body[MAX_BODY_BYTES];
    int body_len = mg_read(conn, body, sizeof(body) - 1);
    if (body_len < 0) {
        body_len = 0;
    }
    body[body_len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        mg_send_http_error(conn, 400, "Body must be valid JSON");
        return 1;
    }

    char *pretty = cJSON_Print(json);
    printf("Instruction from frontend:\n%s\n\n", pretty);
    fflush(stdout);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", 1);
    cJSON_AddItemToObject(response, "received", json);
    char *response_text = cJSON_PrintUnformatted(response);

    mg_send_http_ok(conn, "application/json", (long long)strlen(response_text));
    mg_write(conn, response_text, strlen(response_text));

    free(pretty);
    free(response_text);
    cJSON_Delete(response); /* also frees the "json" object attached above */

    return 1;
}

int main(void) {
    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

    mg_init_library(0);

    const char *options[] = {"listening_ports", LISTEN_PORT, "num_threads", "4", NULL};

    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    struct mg_context *ctx = mg_start(&callbacks, NULL, options);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to start server on port %s\n", LISTEN_PORT);
        return 1;
    }

    mg_set_request_handler(ctx, "/api/command", command_handler, NULL);

    printf("Remotica backend listening on http://0.0.0.0:%s\n", LISTEN_PORT);
    printf("Waiting for instructions from the frontend...\n\n");
    fflush(stdout);

    while (!s_stop_requested) {
        sleep(1);
    }

    printf("\nShutting down...\n");
    mg_stop(ctx);
    mg_exit_library();

    return 0;
}
