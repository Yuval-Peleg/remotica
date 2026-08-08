#ifndef REMOTICA_CAMERA_H
#define REMOTICA_CAMERA_H

/*
 * camera.h
 * =========
 * Optional webcam support: detects a usable V4L2 (Video4Linux2, Linux's
 * standard camera API) capture device and, if one is found, streams it
 * to the frontend as MJPEG over HTTP — the same "multipart/x-mixed-
 * replace" format OctoPrint and mjpg-streamer use, which browsers can
 * display directly with a plain `<img src="/api/camera/stream">`, no
 * client-side video-decoding JavaScript needed.
 *
 * *** VERIFIED AGAINST REAL FRAME CAPTURE (2026-08-08) ***
 * Both halves are now confirmed against a real UVC webcam. Device
 * discovery (querying a camera's name/capabilities via
 * VIDIOC_QUERYCAP/VIDIOC_ENUM_FMT) is a passive metadata read — it does
 * NOT turn the camera on or capture any image. The mmap capture loop and
 * the MJPEG multipart stream are confirmed too: a 3-second request to
 * GET /api/camera/stream produced 24 parts, each a well-formed 640x480
 * JPEG (correct SOI/EOI markers), with frameSeq advancing in step.
 *
 * Still worth checking the picture actually shows something sensible
 * before trusting it unattended, and note the privacy dimension that
 * shapes the design below: turning a camera on — unlike querying a 3D
 * printer's firmware — can see into wherever it's pointed.
 *
 * For that same reason, capture is LAZY: camera_init() only discovers
 * whether a usable camera exists (a passive check, safe to run on every
 * startup) — it does not open or activate it. The camera only actually
 * turns on the first time a client requests GET /api/camera/stream (see
 * ensure_capture_started() in camera.c), not just because the backend
 * process happens to be running. Once started it keeps running for the
 * rest of the process's life (there's no reference-counted "stop when
 * the last viewer leaves" — see camera_shutdown() for the only other
 * way it stops), so opening the camera view even once means it stays
 * active in the background afterward, same as any other webcam app.
 *

 * Scope: only supports cameras that natively output MJPEG-compressed
 * frames over V4L2 (V4L2_PIX_FMT_MJPEG). This covers the large majority
 * of real USB webcams — UVC-class devices normally support MJPEG
 * specifically because raw video at a useful resolution needs more USB
 * bandwidth than most links comfortably provide — and it means this
 * module can just relay already-compressed frames straight from the
 * driver to the HTTP client with no JPEG encoding of its own. A camera
 * that only offers raw formats (YUYV etc.) is reported as detected-but-
 * unusable rather than attempting to encode JPEG here: that would be a
 * meaningfully larger undertaking (and a new third-party dependency,
 * unlike everything else in this project) for a secondary feature.
 */

struct mg_context;

/* Scans /dev/video* for a usable camera (see the scope note above) and
 * remembers it if found. Does NOT open or turn the camera on — see the
 * "capture is LAZY" note above. Safe to call even if no camera is
 * present or usable — GET /api/camera will just report
 * {"available":false}, and GET /api/camera/stream will 503. Call once
 * at startup, any time before camera_register_routes(). */
void camera_init(void);

/* Stops the capture thread (if running) and closes the camera. Call
 * once at shutdown, BEFORE mg_stop() — camera_stream_handler's loop
 * checks for the camera having gone away and exits promptly when it
 * does, but only mg_stop() actually waits for that civetweb worker
 * thread to finish; calling this after mg_stop() would mean mg_stop()
 * blocks until every currently-streaming browser tab happens to
 * disconnect on its own, which could be indefinitely. Safe to call even
 * if camera_init() never found a camera. */
void camera_shutdown(void);

/* Registers GET /api/camera (JSON: {"available": bool, "name": string})
 * and GET /api/camera/stream (the MJPEG stream itself) on `ctx`. */
void camera_register_routes(struct mg_context *ctx);

#endif /* REMOTICA_CAMERA_H */
