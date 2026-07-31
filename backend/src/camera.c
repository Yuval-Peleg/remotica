/*
 * camera.c
 * =========
 * See camera.h for the overview and the important warning about frame
 * capture being unverified against real hardware. This file is the
 * "how": V4L2 device discovery, mmap-based capture, and serving the
 * result as an MJPEG multipart HTTP stream.
 */

/* -std=c99 hides several POSIX functions (usleep(), strdup()) unless we
 * ask glibc for its normal feature set first — same reason every other
 * file in this backend that needs them does this, must come before any
 * system header is included. */
#define _DEFAULT_SOURCE

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cJSON.h"
#include "civetweb.h"

/* Buffers requested from the driver for mmap streaming I/O — 4 is a
 * conservative, commonly-used value: enough to keep the capture loop
 * from stalling if it's briefly a little slow to keep up, without
 * wasting much memory (each buffer holds one compressed JPEG frame, not
 * a full raw frame). */
#define CAPTURE_BUFFER_COUNT 4

/* Preferred capture resolution — a modest starting point, not a hard
 * requirement: VIDIOC_S_FMT is a negotiation, the driver is free to
 * substitute its own closest supported size, and this code uses
 * whatever it actually agreed to (see g_camera.width/height) rather than
 * assuming this exact value stuck. */
#define PREFERRED_WIDTH 640
#define PREFERRED_HEIGHT 480

/* How long the capture thread's poll() waits for a frame before looping
 * back around to check stop_requested again — generous relative to any
 * real camera's frame interval (a working camera hands back a frame
 * well under 100ms after being asked), short enough that
 * camera_shutdown() doesn't take long to notice a stopped/unplugged
 * camera and give up. */
#define FRAME_POLL_TIMEOUT_MS 2000

/* How often camera_stream_handler checks for a new frame to send. Not a
 * frame-rate cap in the usual sense — it only ever actually sends a
 * frame once latest_frame_seq has actually advanced (see below) — just
 * how promptly a new frame gets noticed. Comfortably under any real
 * camera's frame interval. */
#define STREAM_POLL_INTERVAL_MS 30

#define MJPEG_BOUNDARY "remoticacameraframe"

typedef struct {
    void *start;
    size_t length;
} MappedBuffer;

/* Everything about the camera, if one was found — a single global
 * instance is deliberate, the same reasoning as WsBroadcaster/ConsoleLog
 * being owned once by main.c: there's exactly one camera concept this
 * backend cares about, not per-connection state. See each field's own
 * comment for its locking rules — this struct has two different locks
 * for two different reasons, not one lock for everything. */
static struct {
    pthread_mutex_t lock;

    /* Filled in once by camera_init() from a passive discovery scan —
     * device_path[0] == '\0' means no usable camera was found at all.
     * Finding one does NOT open it or turn it on; see start_lock below
     * for what actually does. */
    char device_path[64];
    char name[128]; /* from VIDIOC_QUERYCAP's "card" field */

    /* Deliberately separate from `lock`: actually starting capture
     * (open + setup_capture + spawning the capture thread) is a whole
     * sequence of steps that has to run as one atomic unit — held for
     * that whole sequence, not just a single field read/write, which
     * `lock` is used for elsewhere. See ensure_capture_started(). Two
     * viewers requesting the stream for the very first time at nearly
     * the same moment must not both try to open and start streaming the
     * same device concurrently (most V4L2 drivers only support one
     * active streaming session at a time) — holding this for the whole
     * start sequence makes the second caller simply wait for the first
     * to finish, then see capture_started already true and skip
     * straight to "already running". Safe to hold for the whole
     * sequence specifically because every step in it is fast/local
     * (ioctls, mmap, pthread_create — no network waits), unlike, say,
     * printer_state_to_json()'s "don't hold the lock during slow work"
     * concern elsewhere in this codebase. */
    pthread_mutex_t start_lock;
    int capture_started; /* 0 until the first stream request actually turns the camera on —
                          * see camera.h's privacy note on why this isn't done eagerly at
                          * startup */

    int fd;            /* -1 until capture_started */
    int width, height; /* whatever the driver actually agreed to in setup_capture() */

    /* Guarded by `lock`: the capture thread writes latest_frame/
     * latest_frame_size/latest_frame_seq under it on every new frame,
     * and the stream handler reads them under it to serve whatever's
     * freshest. This is deliberately a "keep only the newest frame"
     * model, not a per-client queue — every connected viewer just gets
     * whatever's freshest whenever it polls, which is exactly what a
     * live camera preview wants (nobody needs to catch up on old
     * frames), and it means N simultaneous viewers don't need N
     * separate captures from the camera. */
    unsigned char *latest_frame; /* malloc'd; replaced (not appended to) on every new frame */
    size_t latest_frame_size;
    long latest_frame_seq;

    MappedBuffer buffers[CAPTURE_BUFFER_COUNT];
    int buffer_count; /* how many of `buffers` the driver actually granted — can be less
                       * than CAPTURE_BUFFER_COUNT, see setup_capture() */

    pthread_t capture_thread;
    int capture_thread_running;
    volatile int stop_requested;
} g_camera = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .start_lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
};

/* ---------------------------------------------------------------------
 * Device discovery — a passive metadata read, does NOT turn the camera
 * on or capture anything. See camera.h's warning for why that
 * distinction matters here.
 * --------------------------------------------------------------------- */

/* Checks whether `device_path` is a V4L2 device that can (a) capture
 * video at all and (b) do so in MJPEG format — see camera.h for why
 * MJPEG specifically. Returns 1 and fills `out_name` if so, 0 otherwise.
 * Always closes the fd it opens either way — this is a detection-only
 * probe, not the connection the capture thread will actually use. */
static int probe_camera_device(const char *device_path, char *out_name, size_t out_name_size) {
    int fd = open(device_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        close(fd);
        return 0;
    }

    /* Some UVC cameras expose more than one /dev/videoN node for the
     * same physical camera (one for actual video capture, others for
     * metadata/control streams) — only the one that actually reports
     * V4L2_CAP_VIDEO_CAPTURE is useful here. */
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return 0;
    }

    /* Does it offer MJPEG as a capture format? Enumerate formats rather
     * than just trying to set MJPEG and seeing if it sticks, so "doesn't
     * support MJPEG at all" can be told apart from "supports it but
     * something else is wrong" later in setup_capture(). */
    int supports_mjpeg = 0;
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0; fmtdesc.index++) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG) {
            supports_mjpeg = 1;
            break;
        }
    }

    close(fd);

    if (!supports_mjpeg) {
        return 0;
    }

    snprintf(out_name, out_name_size, "%s", cap.card);
    return 1;
}

/* Scans /dev/video* (glob sorts matches, so /dev/video0 is tried before
 * /dev/video1 etc. — deterministic, not that it matters much) for the
 * first device that passes probe_camera_device(). Returns a malloc'd
 * copy of the winning device path (caller must free()) and fills
 * `out_name`, or returns NULL if nothing qualified. */
static char *discover_camera_device(char *out_name, size_t out_name_size) {
    /* Zero-initialized so globfree() below is always safe to call even
     * if glob() fails without touching gl_pathc/gl_pathv — same
     * reasoning as transport_serial_discover()'s identical pattern. */
    glob_t matches = {0};
    if (glob("/dev/video*", 0, NULL, &matches) != 0) {
        globfree(&matches);
        return NULL;
    }

    char *found = NULL;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        if (probe_camera_device(matches.gl_pathv[i], out_name, out_name_size)) {
            found = strdup(matches.gl_pathv[i]);
            break;
        }
    }

    globfree(&matches);
    return found;
}

/* ---------------------------------------------------------------------
 * Capture setup/teardown (mmap streaming I/O — the standard, efficient
 * V4L2 capture method: the driver fills buffers mapped directly into
 * our address space, no extra copy through a read() syscall).
 * --------------------------------------------------------------------- */

static int setup_capture(int fd) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = PREFERRED_WIDTH;
    fmt.fmt.pix.height = PREFERRED_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    /* VIDIOC_S_FMT is a negotiation: the driver may substitute its own
     * closest supported width/height rather than erroring out — reading
     * `fmt` back afterward, not assuming our request stuck exactly, is
     * deliberate. We do still require it kept MJPEG specifically; a
     * driver that silently switched to a raw format isn't usable here
     * (see camera.h's scope note). */
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        fprintf(stderr, "camera: VIDIOC_S_FMT failed: %s\n", strerror(errno));
        return -1;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "camera: driver did not keep MJPEG format after VIDIOC_S_FMT\n");
        return -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = CAPTURE_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        fprintf(stderr, "camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        /* Need at least a couple of buffers to keep the capture pipeline
         * from stalling; a driver that can't grant that isn't usable. */
        fprintf(stderr, "camera: driver only granted %u capture buffer(s)\n", req.count);
        return -1;
    }

    g_camera.buffer_count = (int)req.count;
    for (int i = 0; i < g_camera.buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned int)i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            fprintf(stderr, "camera: VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            return -1;
        }

        void *start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (start == MAP_FAILED) {
            fprintf(stderr, "camera: mmap failed: %s\n", strerror(errno));
            return -1;
        }
        g_camera.buffers[i].start = start;
        g_camera.buffers[i].length = buf.length;

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            fprintf(stderr, "camera: VIDIOC_QBUF failed: %s\n", strerror(errno));
            return -1;
        }
    }

    g_camera.width = fmt.fmt.pix.width;
    g_camera.height = fmt.fmt.pix.height;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        fprintf(stderr, "camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* Unmaps every buffer setup_capture() mapped. Does NOT close `fd` — the
 * caller owns that (see camera_init()/camera_shutdown()). Every step is
 * best-effort: this only ever runs during teardown, when there's nothing
 * more useful to do than continue cleaning up even if one step fails. */
static void teardown_capture(int fd) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < g_camera.buffer_count; i++) {
        if (g_camera.buffers[i].start != NULL && g_camera.buffers[i].start != MAP_FAILED) {
            munmap(g_camera.buffers[i].start, g_camera.buffers[i].length);
            g_camera.buffers[i].start = NULL;
        }
    }
    g_camera.buffer_count = 0;
}

/* ---------------------------------------------------------------------
 * Capture thread
 * --------------------------------------------------------------------- */

static void *capture_thread_main(void *arg) {
    (void)arg;
    /* Fixed for this thread's whole life — only camera_shutdown() ever
     * changes g_camera.fd, and it does so only AFTER joining this
     * thread, so there's no race reading it once here up front. */
    int fd = g_camera.fd;

    while (!g_camera.stop_requested) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int poll_result = poll(&pfd, 1, FRAME_POLL_TIMEOUT_MS);
        if (poll_result <= 0) {
            continue; /* timeout, or a transient error — stop_requested is what actually
                       * ends the loop, so just try again rather than treating this as fatal */
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            if (errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "camera: VIDIOC_DQBUF failed: %s — stopping capture\n",
                    strerror(errno));
            break;
        }

        pthread_mutex_lock(&g_camera.lock);
        unsigned char *copy = malloc(buf.bytesused);
        if (copy != NULL) {
            memcpy(copy, g_camera.buffers[buf.index].start, buf.bytesused);
            free(g_camera.latest_frame);
            g_camera.latest_frame = copy;
            g_camera.latest_frame_size = buf.bytesused;
            g_camera.latest_frame_seq++;
        }
        pthread_mutex_unlock(&g_camera.lock);

        /* Hand the buffer back to the driver so it can reuse it for a
         * future frame — best-effort; if this fails the driver will
         * just run short on buffers, not corrupt anything. */
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    return NULL;
}

/* ---------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void camera_init(void) {
    char name[sizeof(g_camera.name)];
    name[0] = '\0';

    char *device_path = discover_camera_device(name, sizeof(name));
    if (device_path == NULL) {
        printf("No usable camera found (checked /dev/video* for an MJPEG-capable capture "
               "device).\n");
        return;
    }

    /* Deliberately does NOT open or activate the camera here — that only
     * happens lazily, the first time a client actually requests the
     * stream (see ensure_capture_started()). Discovery alone means "a
     * camera is plugged in and looks usable," not "start pulling real
     * images from it" — see camera.h's privacy note for why that
     * distinction matters. */
    pthread_mutex_lock(&g_camera.lock);
    snprintf(g_camera.device_path, sizeof(g_camera.device_path), "%s", device_path);
    snprintf(g_camera.name, sizeof(g_camera.name), "%s", name);
    pthread_mutex_unlock(&g_camera.lock);

    printf("Camera found: %s (%s) — capture starts when a client first requests "
           "/api/camera/stream.\n",
           name, device_path);
    free(device_path);
}

/* Opens the discovered camera and starts the capture thread, unless
 * that's already been done — see start_lock's comment for why this is
 * safe to call from multiple threads at once. Returns 1 if capture is
 * (now, or already) running, 0 if there's no camera or it couldn't be
 * started. Called from camera_stream_handler on every request; the
 * early "already started" check makes repeat calls cheap. */
static int ensure_capture_started(void) {
    pthread_mutex_lock(&g_camera.start_lock);

    if (g_camera.capture_started) {
        pthread_mutex_unlock(&g_camera.start_lock);
        return 1;
    }

    pthread_mutex_lock(&g_camera.lock);
    int has_device = (g_camera.device_path[0] != '\0');
    char device_path[sizeof(g_camera.device_path)];
    snprintf(device_path, sizeof(device_path), "%s", g_camera.device_path);
    pthread_mutex_unlock(&g_camera.lock);

    if (!has_device) {
        pthread_mutex_unlock(&g_camera.start_lock);
        return 0;
    }

    int fd = open(device_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "camera: failed to open %s: %s\n", device_path, strerror(errno));
        pthread_mutex_unlock(&g_camera.start_lock);
        return 0;
    }

    if (setup_capture(fd) != 0) {
        fprintf(stderr, "camera: could not start capture on %s\n", device_path);
        close(fd);
        pthread_mutex_unlock(&g_camera.start_lock);
        return 0;
    }

    pthread_mutex_lock(&g_camera.lock);
    g_camera.fd = fd;
    pthread_mutex_unlock(&g_camera.lock);

    g_camera.stop_requested = 0;
    if (pthread_create(&g_camera.capture_thread, NULL, capture_thread_main, NULL) != 0) {
        fprintf(stderr, "camera: failed to start capture thread\n");
        teardown_capture(fd);
        close(fd);
        pthread_mutex_lock(&g_camera.lock);
        g_camera.fd = -1;
        pthread_mutex_unlock(&g_camera.lock);
        pthread_mutex_unlock(&g_camera.start_lock);
        return 0;
    }

    g_camera.capture_thread_running = 1;
    g_camera.capture_started = 1;
    printf("Camera capture started: %s, %dx%d MJPEG\n", g_camera.name, g_camera.width,
           g_camera.height);

    pthread_mutex_unlock(&g_camera.start_lock);
    return 1;
}

void camera_shutdown(void) {
    if (!g_camera.capture_thread_running) {
        return;
    }

    g_camera.stop_requested = 1;
    pthread_join(g_camera.capture_thread, NULL);
    g_camera.capture_thread_running = 0;

    int fd = g_camera.fd;
    teardown_capture(fd);
    close(fd);

    pthread_mutex_lock(&g_camera.lock);
    g_camera.fd = -1;
    free(g_camera.latest_frame);
    g_camera.latest_frame = NULL;
    pthread_mutex_unlock(&g_camera.lock);
}

/* ---------------------------------------------------------------------
 * REST: GET /api/camera
 * --------------------------------------------------------------------- */

static int camera_info_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);
    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Use GET");
        return 1;
    }

    pthread_mutex_lock(&g_camera.lock);
    /* "available" here means "was discovered and looks usable," not
     * "currently streaming" — capture itself doesn't start until a
     * client actually requests /api/camera/stream (see
     * ensure_capture_started()), so this check must NOT depend on
     * g_camera.fd (which stays -1 until then). */
    int available = (g_camera.device_path[0] != '\0');
    char name[sizeof(g_camera.name)];
    snprintf(name, sizeof(name), "%s", g_camera.name);
    pthread_mutex_unlock(&g_camera.lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", available);
    cJSON_AddStringToObject(root, "name", name);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
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
 * GET /api/camera/stream — MJPEG multipart stream
 * --------------------------------------------------------------------- */

static int camera_stream_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;

    /* This is the one and only place capture actually turns on — see
     * camera.h's privacy note and ensure_capture_started()'s comment. */
    if (!ensure_capture_started()) {
        mg_send_http_error(conn, 503, "No camera available");
        return 1;
    }

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
              "Cache-Control: no-cache, private\r\n"
              "Pragma: no-cache\r\n"
              "Connection: close\r\n"
              "\r\n",
              MJPEG_BOUNDARY);

    long last_sent_seq = -1;

    /* Loops until the client disconnects (mg_write/mg_printf returning
     * a value that doesn't match what should have been written) or the
     * camera goes away (camera_shutdown(), or the capture thread giving
     * up after a device error) — whichever happens first. Every
     * connected browser tab ties up one civetweb worker thread for as
     * long as it stays open, which is why main.c raises num_threads
     * from its previous default — see the comment there. */
    for (;;) {
        unsigned char *frame = NULL;
        size_t frame_size = 0;

        pthread_mutex_lock(&g_camera.lock);
        if (g_camera.fd < 0) {
            pthread_mutex_unlock(&g_camera.lock);
            break;
        }
        if (g_camera.latest_frame_seq != last_sent_seq && g_camera.latest_frame != NULL) {
            frame = malloc(g_camera.latest_frame_size);
            if (frame != NULL) {
                memcpy(frame, g_camera.latest_frame, g_camera.latest_frame_size);
                frame_size = g_camera.latest_frame_size;
            }
            last_sent_seq = g_camera.latest_frame_seq;
        }
        pthread_mutex_unlock(&g_camera.lock);

        if (frame != NULL) {
            int header_len = mg_printf(conn,
                                       "--%s\r\n"
                                       "Content-Type: image/jpeg\r\n"
                                       "Content-Length: %zu\r\n"
                                       "\r\n",
                                       MJPEG_BOUNDARY, frame_size);
            int written = (header_len > 0) ? mg_write(conn, frame, frame_size) : -1;
            free(frame);

            if (header_len <= 0 || written != (int)frame_size || mg_write(conn, "\r\n", 2) != 2) {
                break; /* client disconnected, or a write failed */
            }
        }

        usleep(STREAM_POLL_INTERVAL_MS * 1000);
    }

    return 1;
}

void camera_register_routes(struct mg_context *ctx) {
    mg_set_request_handler(ctx, "/api/camera", camera_info_handler, NULL);
    mg_set_request_handler(ctx, "/api/camera/stream", camera_stream_handler, NULL);
}
