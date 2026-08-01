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
 *   2. Set up the shared PrinterState, load the PrinterProfile, and try
 *      to find/start a webcam (camera.c — entirely optional, absent
 *      hardware just means no camera stream).
 *   3. Create the printer driver (simulated or real serial) and try to
 *      connect it. For a real printer, failing to connect here (none
 *      plugged in yet, wrong port, powered off, ...) is NOT fatal —
 *      Remotica starts up regardless and a background reconnect thread
 *      (see reconnect_thread_main below) keeps retrying until one
 *      answers, so the frontend's connection badge is what tells you
 *      whether a printer is actually there, not whether the backend is
 *      running at all.
 *   4. Start civetweb (the HTTP + WebSocket library) and register every
 *      route from api_handlers.c, ws_broadcaster.c, console_log.c, and
 *      camera.c.
 *   5. Start a background thread that "ticks" the driver and job manager
 *      forward and pushes fresh state to every connected browser, over
 *      and over, until the program is told to stop (Ctrl+C) — and, for a
 *      real printer, the reconnect thread mentioned above.
 *   6. On shutdown, tear everything down in the reverse of the order it
 *      was started: tick + reconnect threads, then civetweb, then any
 *      running print, and only then the printer connection itself (see
 *      the comment on that block for why that order specifically).
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
#include "camera.h"
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

/* How often the reconnect thread retries when using --serial (auto or an
 * explicit device) and the printer isn't currently connected — whether
 * that's because none was found yet at startup, or a connect attempt
 * since then failed. Deliberately much coarser than TICK_INTERVAL_MS: a
 * connect attempt itself can take several seconds (boot-reset wait, M115
 * query — see transport_serial.c), so retrying every 300ms would mean
 * back-to-back attempts with no real gap between them. */
#define RECONNECT_RETRY_INTERVAL_SECONDS 5

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
 * Reconnect thread (only started when using --serial — the simulator is
 * always connected, so it never needs this)
 *
 * Remotica used to refuse to start at all if a printer wasn't already
 * plugged in and answering at startup (see the "Not done yet" note this
 * replaced in the root CLAUDE.md). That meant you couldn't start Remotica
 * ahead of time and plug the printer in afterward, and the frontend's
 * connection badge — which already correctly renders "Disconnected" —
 * never had a real chance to prove it worked, since the backend simply
 * wouldn't run in that state. Now main() starts the driver in whatever
 * state connect() leaves it in (connected or not) and this thread keeps
 * retrying in the background for as long as it isn't connected, so
 * plugging the printer in (or fixing whatever was wrong — wrong port,
 * powered off, etc.) any time after startup gets picked up automatically,
 * with no restart needed.
 * --------------------------------------------------------------------- */

static void *reconnect_thread_main(void *arg) {
    PrinterDriver *driver = (PrinterDriver *)arg;

    while (!s_stop_requested) {
        printer_state_lock(driver->state);
        int already_connected = driver->state->connected;
        printer_state_unlock(driver->state);

        if (!already_connected && driver->connect(driver) == 0) {
            printf("Printer connected.\n");
            fflush(stdout);
        }

        /* Slept in short chunks, not one long sleep(), so a shutdown
         * request is noticed within a fraction of a second instead of
         * waiting out whatever's left of the retry interval. */
        for (int waited_ms = 0;
             waited_ms < RECONNECT_RETRY_INTERVAL_SECONDS * 1000 && !s_stop_requested;
             waited_ms += 200) {
            usleep(200 * 1000);
        }
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

    /* --port <n> overrides LISTEN_PORT. Not something a normal run needs
     * (the frontend's dev proxy and the eventual single-process build
     * both assume the default), but it lets a second backend — notably
     * backend/tools/fake_marlin_test.py's fake-firmware harness — run
     * against a pty without having to stop a real one already serving
     * the dashboard on 8080. */
    const char *listen_port = LISTEN_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) {
            serial_device = argv[i + 1];
            i++; /* skip the value we just consumed */
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            listen_port = argv[i + 1];
            i++;
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

    /* Independent of the printer driver entirely — scans for a webcam.
     * Only a passive check (see camera.h): actually turning the camera
     * on and capturing frames is deferred until a client first requests
     * the stream, not done just because the backend started. Safe to
     * call even if nothing's found. */
    camera_init();

    /* --- 3. Printer driver --- */

    PrinterDriver *driver;
    if (serial_device != NULL) {
        char discovered_path[64];
        const char *device_to_use = serial_device;
        long baud_rate = SERIAL_BAUD_RATE_DEFAULT;
        /* Set (to a value >= 0) only when discovery found a printer —
         * that already-open, already-booted, already-M115-queried fd
         * gets handed straight to transport_serial_create_from_discovery
         * below instead of opening (and re-booting, and re-querying) the
         * same device a second time from scratch. */
        int discovered_fd = -1;
        char discovered_firmware_info[256] = "";

        if (strcmp(serial_device, "auto") == 0) {
            printf("Scanning for a connected printer (--serial auto)...\n");
            fflush(stdout); /* stdout is fully buffered once it's not a
                             * terminal (e.g. captured by a parent
                             * process), so without this, this message
                             * could print AFTER a subsequent
                             * fprintf(stderr, ...) even though it
                             * happened first — stderr is unbuffered. */
            SerialDiscoveryResult discovered;
            if (transport_serial_discover(&console, &discovered)) {
                snprintf(discovered_path, sizeof(discovered_path), "%s", discovered.device_path);
                device_to_use = discovered_path;
                baud_rate = discovered.baud_rate;
                discovered_fd = discovered.fd;
                snprintf(discovered_firmware_info, sizeof(discovered_firmware_info), "%s",
                         discovered.firmware_info);
                printf("Found a printer at %s (%ld baud).\n", device_to_use, baud_rate);
            } else {
                /* Used to be fatal (return 1) here. Instead, start up
                 * anyway with no device known yet — see the reconnect
                 * thread started further down, which re-scans on a timer
                 * so plugging a printer in later gets picked up without
                 * a restart. */
                printf("No printer found automatically on /dev/ttyACM* or /dev/ttyUSB* — "
                       "starting anyway. Plug one in any time; Remotica will connect "
                       "automatically (retrying every %ds).\n",
                       RECONNECT_RETRY_INTERVAL_SECONDS);
            }
        }

        if (strcmp(serial_device, "auto") == 0 && discovered_fd < 0) {
            /* --serial auto, nothing found above: no device path exists
             * yet to create a normal driver for. This driver re-scans on
             * every connect() attempt instead (see transport_serial_
             * create_auto()). */
            driver = transport_serial_create_auto(&state, &console);
            if (driver == NULL) {
                fprintf(stderr, "Failed to create serial driver\n");
                return 1;
            }
            printf("Using the REAL serial driver (untested against hardware — see "
                   "transport_serial.h for details before trusting this with a real "
                   "printer). No device connected yet.\n");
        } else {
            driver = (discovered_fd >= 0)
                         ? transport_serial_create_from_discovery(&state, &console, device_to_use,
                                                                  baud_rate, discovered_fd,
                                                                  discovered_firmware_info)
                         : transport_serial_create(&state, &console, device_to_use, baud_rate);
            if (driver == NULL) {
                fprintf(stderr, "Failed to create serial driver for %s\n", device_to_use);
                return 1;
            }
            printf(
                "Using the REAL serial driver on %s at %ld baud (untested against hardware —\n"
                "see transport_serial.h for details before trusting this with a real printer).\n",
                device_to_use, baud_rate);
        }
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
        if (serial_device == NULL) {
            /* The simulator's connect() never actually fails today, so
             * this would only trip on a genuine bug in transport_sim.c —
             * worth treating as fatal rather than silently limping on. */
            fprintf(stderr, "Failed to connect to the simulated printer\n");
            return 1;
        }
        /* A real printer not being connected/reachable right now is no
         * longer fatal — see the reconnect thread started below, which
         * keeps retrying in the background so plugging one in (or fixing
         * whatever's wrong) after startup is picked up automatically. */
        printf("Not connected to a printer yet. Remotica is still starting — the frontend's "
               "connection badge will update automatically once one is found.\n");
    }

    /* --- 4. civetweb + routes --- */

    mg_init_library(0);

    /* num_threads used to be 4, which was plenty when every request was
     * quick (jog/home/temp) or handed off to civetweb's own websocket
     * handling (state/console). A camera stream is different: each
     * connected browser tab ties up one civetweb worker thread for as
     * long as it stays open (see camera_stream_handler's loop in
     * camera.c), so a couple of tabs left open with the camera view
     * visible could otherwise starve every other request — jog/home/
     * temp included — of a free thread. 8 leaves real headroom for a
     * few simultaneous camera viewers plus normal API traffic. */
    const char *options[] = {"listening_ports", listen_port, "num_threads", "8", NULL};

    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    struct mg_context *ctx = mg_start(&callbacks, NULL, options);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to start server on port %s\n", listen_port);
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
    camera_register_routes(ctx);

    /* --- 5. Background tick thread + (for a real printer) reconnect thread --- */

    TickThreadArgs tick_args = {.driver = driver, .state = &state, .broadcaster = &broadcaster};
    pthread_t tick_thread;
    pthread_create(&tick_thread, NULL, tick_thread_main, &tick_args);

    /* Only meaningful for the real serial driver — the simulator connects
     * successfully once at startup and stays that way, so there's nothing
     * for this thread to ever do for it. */
    pthread_t reconnect_thread;
    int reconnect_thread_started = 0;
    if (serial_device != NULL) {
        pthread_create(&reconnect_thread, NULL, reconnect_thread_main, driver);
        reconnect_thread_started = 1;
    }

    printf("Remotica backend listening on http://0.0.0.0:%s\n", listen_port);
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
     * tear anything down out from under it. Same for the reconnect
     * thread, if one was started — it also touches the driver (via
     * connect()), so it has to be gone before anything below it runs too.
     * Worst case this pauses for however long a connect() attempt that
     * was already in flight takes to finish (several seconds on real
     * hardware — see BOOT_WAIT_SECONDS/M115 in transport_serial.c), the
     * same kind of bounded shutdown pause job_manager_shutdown() further
     * down already accepts for an in-progress print's safety sequence. */
    pthread_join(tick_thread, NULL);
    if (reconnect_thread_started) {
        pthread_join(reconnect_thread, NULL);
    }

    /* Order matters here, and it's the reverse of the order things were
     * started in — every thread that can touch the driver (or the
     * camera) has to be gone before that connection is torn down.
     *
     * 1. camera_shutdown() first, specifically BEFORE mg_stop(): a
     *    camera stream's civetweb worker thread loops until the camera
     *    goes away or the browser disconnects (see camera_stream_
     *    handler in camera.c) — if mg_stop() ran first, it would block
     *    waiting for that worker thread to finish, which only happens
     *    when EITHER of those things occurs, and a browser tab left open
     *    might never disconnect on its own. Stopping the camera first
     *    makes that loop notice "the camera is gone" and exit promptly,
     *    so mg_stop() afterward has nothing indefinite left to wait on.
     * 2. mg_stop(): until it returns, civetweb's worker threads are
     *    still alive and still serving requests, so a jog/home/temp
     *    request could be sitting inside the driver right now. Closing
     *    the serial fd underneath it would mean writing to a closed
     *    descriptor — or worse, to whatever unrelated file happens to be
     *    opened next and given the same descriptor number. mg_stop()
     *    waits for in-flight requests to finish, so this can pause for a
     *    moment if e.g. a home command is still running; that pause is
     *    the point, not a bug.
     * 3. job_manager_shutdown(): asks any in-progress print streaming
     *    thread to stop and blocks until it actually has (it may spend a
     *    couple of seconds sending its heaters-off safety sequence
     *    first — see send_abort_safety_sequence in job_manager.c).
     * 4. Only now is nothing else using the connection, so it's safe to
     *    close. */
    camera_shutdown();
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
