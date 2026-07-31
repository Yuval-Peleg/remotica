# Remotica

An from-scratch, OctoPrint-inspired remote control dashboard for 3D printers. Dark/sage-green theme throughout. Not ready for real printer use yet — see README.md.

## Repo layout

- `frontend/` — the active app. See `frontend/CLAUDE.md` for details.
- `backend/` — C, using civetweb (embedded HTTP + WebSocket server) and cJSON, both vendored under `backend/third_party/`. See "Backend" below.
- `legacy-static/`, `images/` — old hand-built mockup, kept on disk for reference only. Gitignored on purpose — do not re-add to git without asking.

## Deployment model

Frontend and backend run as a **single process** on the same PC that's physically wired to the printer (same model OctoPrint uses). The backend serves the already-built frontend directly — no separate hosting, no CORS setup. Other devices reach it over LAN by browsing to that PC's IP. No accounts, no cloud relay, no port-forwarding.

## Backend

- **Language:** C — deliberate choice, not a placeholder.
- **HTTP + WebSocket:** [civetweb](https://github.com/civetweb/civetweb) (MIT), one embeddable library for both. **Not Mongoose** — Mongoose looked like the obvious pick early on, but it's dual-licensed GPLv2/commercial with no permissive option, which would force this MIT-licensed repo to either go GPL or pay for a commercial license. civetweb is a permissively-licensed sibling (same original codebase lineage) with the same HTTP+WebSocket feature set, so it was swapped in instead. If any other C dependency gets added later, check its license before vendoring it — same issue could recur.
- **JSON:** [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Both vendored as source under `backend/third_party/` (civetweb 1.16, cJSON 1.7.19) — no package manager, just `.c`/`.h` files compiled straight into the binary. Their upstream LICENSE files are kept alongside them.
- **Build:** `cd backend && make` → `backend/build/remotica-backend`. Run it from `backend/` (it uses relative paths for `data/profile.json` and `data/uploads/`, both gitignored — runtime-generated). Built with `NO_SSL` (no HTTPS needed on LAN) and `USE_WEBSOCKET` defined. Needs a C toolchain — install via `sudo apt-get install build-essential clang-format cppcheck`.
- **Target OS:** Linux first, headless-compatible (Ubuntu Server, no GUI needed). A Windows fork is planned for later; only the serial layer (POSIX `termios` → Win32 serial API) should need to change, since civetweb/cJSON are already cross-platform.

### Source layout (`backend/src/`)

- `main.c` — wires everything together: parses `--serial <device>` (optional — omit it to use the simulator, which is the default), starts civetweb, registers routes, runs a background thread that ticks the driver and broadcasts state roughly every 300ms. On shutdown, calls `job_manager_shutdown()` (stops/joins any in-progress print streaming thread) before disconnecting the driver, so the driver's connection can't be torn down while that thread is still using it.
- `printer_state.h/.c` — `PrinterState`: the single shared, mutex-protected struct holding "what's true about the printer right now" (temps, position, connection, job status/progress — status is idle/ready/printing/paused). Everything else reads/writes through this.
- `printer_profile.h/.c` — bed size / max Z / min extrude temp, persisted to `data/profile.json`. The frontend fetches this from `GET /api/profile` on mount instead of hardcoding it.
- `transport.h` — `PrinterDriver`: the function-pointer interface (connect/disconnect/jog/home/set_target_temp/tick/send_gcode_line) that decouples the REST API and job manager from *how* commands actually reach the printer.
- `transport_sim.h/.c` — the default driver. No real hardware; fakes temperature drift and instant moves. Its `send_gcode_line` does just enough gcode parsing (G0/G1 moves incl. G90/G91/M82/M83 modal state, G28 home, M104/M140/M109/M190 target temps) to make a simulated print visibly drive the position/temperature UI, not just log lines.
- `transport_serial.h/.c` — the real driver, talks POSIX `termios` + G-code to an actual printer over USB. **Written carefully but not tested against real hardware** (none was available while writing it) — see the warning at the top of `transport_serial.h` before using `--serial` with a real printer. An Opus review (2026-07-31) found and fixed several serious concurrency/protocol bugs in this file (no lock serializing the three threads that touch the serial port, timeouts that could hang forever, stale-"ok" desync risk) — see git history around that date for the review and fix commits if you need the details. Print-streamed gcode lines are sent with a RepRap-standard line number + checksum (`gcode_checksum`/`send_checksummed_line`), negotiated at connect time via `M110 N0` with a clean fallback to plain lines for firmware that doesn't support it — catches bit corruption in transit (retried via the printer's `Resend:` reply) instead of silently executing a corrupted command. Verified against a real Linux pty pair standing in for hardware (a small Python fake-firmware script), since there's still no real printer to test against — see git history for that test script if useful. Its `send_gcode_line` doesn't track position from the gcode file it streams (no reliable firmware-agnostic way to query it back) — position readouts are known to lag reality during a real print.
- `job_manager.h/.c` — gcode upload (to `data/uploads/`, with filename validation against path traversal), the print job state machine (idle → ready → printing ⇄ paused → ready), and "on device" file management (list/select/delete previously-uploaded files — files are kept on disk until explicitly deleted, so they can be reprinted without re-uploading). **Real print streaming**: `job_manager_start_print()` launches a dedicated background thread (`streamer_thread_main`) that reads the queued gcode file line by line (stripping blank/comment lines), sends each real line to the driver via `send_gcode_line()`, and tracks progress as lines-sent/total-lines — pause/resume/cancel are all handled by the thread polling `state->job.status` and a small cancel flag between lines. Runs on its own thread (not the shared tick loop) specifically so a multi-hour print doesn't block temperature polling or WebSocket broadcasts. Uploading, selecting, jogging, or homing while a print is running/paused is refused with 409 (see `api_handlers.c`) — those used to be able to silently kill or corrupt an in-progress print. Cancelling or losing the printer mid-print now runs a safety sequence (`send_abort_safety_sequence`: heaters off, fan off, small Z lift, steppers disabled) before the job settles into its final status.
- `api_handlers.h/.c` — every REST route: `GET /api/state`, `POST /api/jog`, `POST /api/home`, `POST /api/temp`, `GET`+`POST /api/profile`, `POST /api/upload?filename=...`, `POST /api/print/start`, `POST /api/print/pause`, `POST /api/print/resume`, `POST /api/print/cancel`, `GET /api/files`, `GET /api/files/content?filename=...`, `POST /api/files/select?filename=...`, `POST /api/files/delete?filename=...`. Also where jog/temp inputs get clamped to the profile's physical limits and the cold-extrusion safety check lives (mirrors the frontend's `canExtrude` check, enforced server-side too since a backend shouldn't trust a frontend-only safety check for something with a physical consequence).
- `ws_broadcaster.h/.c` — tracks connected WebSocket clients at `/api/ws` and pushes a state snapshot (same shape as `GET /api/state`) to all of them every tick.

The original `POST /api/command` smoke-test route and its frontend dev panel (`BackendConnectionTest.jsx`) have been removed now that the frontend talks to the real endpoints above.

### Not done yet

- **The serial driver is unverified against real hardware** (see above) — this matters more now that a real print would actually stream real gcode to it line by line, not just jog/home/temp commands.
- **The backend doesn't serve the built frontend yet** — right now they're two separate processes talking over the Vite dev proxy (`frontend/vite.config.js`, `ws: true` for `/api/ws` too). The "single process" deployment model (see above) still needs civetweb's static-file serving wired up to `frontend/dist/` for a production build.

## Formatting / static analysis

- **C:** `.clang-format` at repo root (LLVM base, 4-space indent, 100 col). Run `clang-format -i backend/src/*.c` (don't run it on `backend/third_party/` — that's vendored upstream code, leave it as-is). Use `cppcheck backend/src/` for static analysis. None of `clang-format`/`cppcheck`/a C toolchain are installed yet — install via `sudo apt-get install build-essential clang-format cppcheck`.
- **Frontend:** see `frontend/CLAUDE.md`.

## Git

- `frontend/`, `backend/` (except `backend/build/` and `backend/data/`, both gitignored), and root docs/config are tracked. `legacy-static/` and `images/` are gitignored on purpose.
- Git identity is already configured (`Yuval Peleg`, a GitHub noreply email) — **never run `git config` to change it**, that email was deliberately chosen.
- Repo is public: https://github.com/Yuval-Peleg/remotica — README explicitly warns it's not ready for real printer use; MIT LICENSE backs that disclaimer.
- **Committing and pushing to this repo is pre-authorized** (confirmed 2026-07-31) — no need to ask before each commit/push, use judgment on when a chunk of work is worth committing and split into logical commits rather than one giant one.
