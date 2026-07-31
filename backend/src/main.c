/*
 * main.c
 * =======
 * The entry point: this is where everything built in the other files
 * gets wired together into one running program.
 *
 * What happens, in order:
 *   1. Parse command-line arguments (currently just an optional
 *      --serial <device> to talk to a real printer instead of the
 *      built-in simulator).
 *   2. Set up the shared PrinterState and load the PrinterProfile.
 *   3. Create the printer driver (simulated or real serial) and connect
 *      it.
 *   4. Start civetweb (the HTTP + WebSocket library) and register every
 *      route from api_handlers.c and the WebSocket endpoint from
 *      ws_broadcaster.c.
 *   5. Start a background thread that "ticks" the driver and job manager
 *      forward and pushes fresh state to every connected browser, over
 *      and over, until the program is told to stop (Ctrl+C).
 *   6. On shutdown, tear everything down in the reverse of the order it
 *      was started: tick thread, then civetweb, then any running print,
 *      and only then the printer connection itself (see the comment on
 *      that block for why that order specifically).
 */

/* See the identical comment in transport_serial.c: -std=c99 hides some
 * POSIX functions (usleep() here) unless we ask glibc for its normal
 * feature set first, before any system header is included. */
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "api_handlers.h"
#include "civetweb.h"
#include "console_log.h"
#include "job_manager.h"
#include "printer_profile.h"
#include "printer_state.h"
#include "transport.h"
#include "transport_serial.h"
#include "transport_sim.h"
#include "ws_broadcaster.h"

#define LISTEN_PORT "8080"

/* Where the printer profile is persisted, and where uploaded gcode files
 * are stored. Both are relative paths, so run this program with
 * backend/ as the current directory (that's what `make run` and the
 * README instructions do) — see .gitignore, both of these are
 * runtime-generated and intentionally not committed to the repo. */
#define PROFILE_PATH "data/profile.json"
#define UPLOADS_DIR "data/uploads"

/* How often the background tick thread wakes up to advance the
 * simulation/poll the real printer and broadcast state. 300ms is
 * frequent enough that the frontend's live temperature graph and
 * position readouts feel responsive, without being so frequent that it
 * wastes CPU or (for the real serial driver) floods the printer with
 * M105 requests. */
#define TICK_INTERVAL_MS 300

static volatile sig_atomic_t s_stop_requested = 0;

static void handle_stop_signal(int sig_num) {
    (void)sig_num;
    s_stop_requested = 1;
}

/* ---------------------------------------------------------------------
 * Background tick thread
 * --------------------------------------------------------------------- */

typedef struct {
    PrinterDriver *driver;
    PrinterState *state;
    WsBroadcaster *broadcaster;
} TickThreadArgs;

static void *tick_thread_main(void *arg) {
    TickThreadArgs *args = (TickThreadArgs *)arg;

    while (!s_stop_requested) {
        args->driver->tick(args->driver);
        ws_broadcaster_send_state(args->broadcaster, args->state);

        usleep(TICK_INTERVAL_MS * 1000);
    }

    return NULL;
}

/* ---------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* --- 1. Command-line arguments --- */

    /* If --serial <device> is given, we'll talk to a real printer over
     * that serial device (e.g. --serial /dev/ttyUSB0). --serial auto
     * instead has the backend scan for one itself (see
     * transport_serial_discover()) rather than needing an exact device
     * path up front. Without --serial at all, we default to the
     * simulator, which is the safe choice and doesn't require any
     * hardware — see transport_serial.h for an important warning about
     * the serial driver being untested against real hardware before you
     * reach for --serial. */
    const char *serial_device = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) {
            serial_device = argv[i + 1];
            i++; /* skip the value we just consumed */
        }
    }

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

    /* --- 2. Shared state + profile --- */

    PrinterState state;
    printer_state_init(&state);

    PrinterProfile profile;
    printer_profile_load(&profile, PROFILE_PATH);

    ConsoleLog console;
    console_log_init(&console);

    /* --- 3. Printer driver --- */

    PrinterDriver *driver;
    if (serial_device != NULL) {
        char discovered_path[64];
        const char *device_to_use = serial_device;
        long baud_rate = SERIAL_BAUD_RATE_DEFAULT;

        if (strcmp(serial_device, "auto") == 0) {
            printf("Scanning for a connected printer (--serial auto)...\n");
            fflush(stdout); /* stdout is fully buffered once it's not a
                             * terminal (e.g. captured by a parent
                             * process), so without this, this message
                             * could print AFTER a subsequent
                             * fprintf(stderr, ...) even though it
                             * happened first — stderr is unbuffered. */
            SerialDiscoveryResult discovered;
            if (!transport_serial_discover(&console, &discovered)) {
                fprintf(stderr, "No printer found automatically on /dev/ttyACM* or "
                                "/dev/ttyUSB*. Plug it in, or pass an explicit --serial "
                                "<device> instead.\n");
                return 1;
            }
            snprintf(discovered_path, sizeof(discovered_path), "%s", discovered.device_path);
            device_to_use = discovered_path;
            baud_rate = discovered.baud_rate;
            printf("Found a printer at %s (%ld baud).\n", device_to_use, baud_rate);
        }

        driver = transport_serial_create(&state, &console, device_to_use, baud_rate);
        if (driver == NULL) {
            fprintf(stderr, "Failed to create serial driver for %s\n", device_to_use);
            return 1;
        }
        printf("Using the REAL serial driver on %s at %ld baud (untested against hardware —\n"
               "see transport_serial.h for details before trusting this with a real printer).\n",
               device_to_use, baud_rate);
    } else {
        driver = transport_sim_create(&state, &console);
        if (driver == NULL) {
            fprintf(stderr, "Failed to create simulated driver\n");
            return 1;
        }
        printf("Using the simulated printer driver (pass --serial <device> to use a real "
               "printer instead).\n");
    }

    if (driver->connect(driver) != 0) {
        fprintf(stderr, "Failed to connect to the printer\n");
        return 1;
    }

    /* --- 4. civetweb + routes --- */

    mg_init_library(0);

    const char *options[] = {"listening_ports", LISTEN_PORT, "num_threads", "4", NULL};

    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    struct mg_context *ctx = mg_start(&callbacks, NULL, options);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to start server on port %s\n", LISTEN_PORT);
        driver->disconnect(driver);
        return 1;
    }

    AppContext app_context = {
        .state = &state,
        .driver = driver,
        .profile = &profile,
        .profile_path = PROFILE_PATH,
        .uploads_dir = UPLOADS_DIR,
    };
    api_handlers_register_all(ctx, &app_context);

    WsBroadcaster broadcaster;
    ws_broadcaster_init(&broadcaster);
    ws_broadcaster_register(ctx, &broadcaster);

    console_log_register_routes(ctx, &console);

    /* --- 5. Background tick thread --- */

    TickThreadArgs tick_args = {.driver = driver, .state = &state, .broadcaster = &broadcaster};
    pthread_t tick_thread;
    pthread_create(&tick_thread, NULL, tick_thread_main, &tick_args);

    printf("Remotica backend listening on http://0.0.0.0:%s\n", LISTEN_PORT);
    printf("REST API under /api/*, live state over WebSocket at /api/ws\n");
    printf("Gcode console: GET /api/console, live at /api/ws/console\n\n");
    fflush(stdout);

    while (!s_stop_requested) {
        sleep(1);
    }

    /* --- 6. Shutdown --- */

    printf("\nShutting down...\n");

    /* s_stop_requested is already set (that's what got us out of the loop
     * above), so the tick thread will see it on its next wake-up and
     * return — pthread_join waits for that to actually happen before we
     * tear anything down out from under it. */
    pthread_join(tick_thread, NULL);

    /* Order matters here, and it's the reverse of the order things were
     * started in — every thread that can touch the driver has to be gone
     * before the driver's connection is torn down.
     *
     * 1. mg_stop() first: until it returns, civetweb's worker threads are
     *    still alive and still serving requests, so a jog/home/temp
     *    request could be sitting inside the driver right now. Closing
     *    the serial fd underneath it would mean writing to a closed
     *    descriptor — or worse, to whatever unrelated file happens to be
     *    opened next and given the same descriptor number. mg_stop()
     *    waits for in-flight requests to finish, so this can pause for a
     *    moment if e.g. a home command is still running; that pause is
     *    the point, not a bug.
     * 2. job_manager_shutdown(): asks any in-progress print streaming
     *    thread to stop and blocks until it actually has (it may spend a
     *    couple of seconds sending its heaters-off safety sequence
     *    first — see send_abort_safety_sequence in job_manager.c).
     * 3. Only now is nothing else using the connection, so it's safe to
     *    close. */
    mg_stop(ctx);
    job_manager_shutdown();
    driver->disconnect(driver);
    mg_exit_library();

    if (serial_device != NULL) {
        transport_serial_destroy(driver);
    } else {
        transport_sim_destroy(driver);
    }

    return 0;
}
