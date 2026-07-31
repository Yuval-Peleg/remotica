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

- `main.c` — wires everything together: parses `--serial <device>` (optional — omit it to use the simulator, which is the default), starts civetweb, registers routes, runs a background thread that ticks the driver/job manager and broadcasts state roughly every 300ms.
- `printer_state.h/.c` — `PrinterState`: the single shared, mutex-protected struct holding "what's true about the printer right now" (temps, position, connection, job status/progress). Everything else reads/writes through this.
- `printer_profile.h/.c` — bed size / max Z / min extrude temp, persisted to `data/profile.json`. Same field names as the frontend's `printer-profile.js` (still hardcoded there — not yet wired to fetch this from the backend).
- `transport.h` — `PrinterDriver`: the function-pointer interface (connect/disconnect/jog/home/set_target_temp/tick) that decouples the REST API from *how* commands actually reach the printer.
- `transport_sim.h/.c` — the default driver. No real hardware; fakes temperature drift and instant moves, mirroring `use-printer-temps.js`'s mock behavior in C.
- `transport_serial.h/.c` — the real driver, talks POSIX `termios` + G-code to an actual printer over USB. **Written carefully but not tested against real hardware** (none was available while writing it) — see the warning at the top of `transport_serial.h` before using `--serial` with a real printer. This is exactly the kind of code to run past Opus or test very cautiously first (see the project's memory notes on when to do that).
- `job_manager.h/.c` — gcode upload (to `data/uploads/`, with filename validation against path traversal) and the print job state machine (idle → ready → printing → ready). Progress while "printing" is a **fake timer** (~30s per print), not real line-by-line streaming to the printer yet — see the big comment on `job_manager_tick()` for what real streaming would need.
- `api_handlers.h/.c` — every REST route: `GET /api/state`, `POST /api/jog`, `POST /api/home`, `POST /api/temp`, `GET`+`POST /api/profile`, `POST /api/upload?filename=...`, `POST /api/print/start`, `POST /api/print/cancel`. Also where jog/temp inputs get clamped to the profile's physical limits and the cold-extrusion safety check lives (mirrors `ControlPanel.jsx`'s `canExtrude` check, enforced server-side too since a backend shouldn't trust a frontend-only safety check for something with a physical consequence).
- `ws_broadcaster.h/.c` — tracks connected WebSocket clients at `/api/ws` and pushes a state snapshot (same shape as `GET /api/state`) to all of them every tick.
- `POST /api/command` (defined directly in `main.c`) — the original connectivity smoke-test route, kept for `BackendConnectionTest.jsx`. Delete both together once the frontend is wired to the real endpoints above.

### Not done yet

- **Frontend isn't wired to any of this** — `use-printer-temps.js`, `ControlPanel.jsx`'s local jog state, and `printer-profile.js` are all still frontend-only mock data. The backend now has everything they'd need to talk to instead (`GET /api/state`, `POST /api/jog`, etc., plus `/api/ws` for live updates) — wiring that up is the natural next step.
- **Real print streaming** — `job_manager_tick()`'s progress is a fake timer, not derived from actually sending the queued gcode file to the printer line by line. See that function's doc comment for what's involved.
- **The serial driver is unverified against real hardware** (see above).

## Formatting / static analysis

- **C:** `.clang-format` at repo root (LLVM base, 4-space indent, 100 col). Run `clang-format -i backend/src/*.c` (don't run it on `backend/third_party/` — that's vendored upstream code, leave it as-is). Use `cppcheck backend/src/` for static analysis. None of `clang-format`/`cppcheck`/a C toolchain are installed yet — install via `sudo apt-get install build-essential clang-format cppcheck`.
- **Frontend:** see `frontend/CLAUDE.md`.

## Git

- `frontend/`, `backend/` (except `backend/build/` and `backend/data/`, both gitignored), and root docs/config are tracked. `legacy-static/` and `images/` are gitignored on purpose.
- Git identity is already configured (`Yuval Peleg`, a GitHub noreply email) — **never run `git config` to change it**, that email was deliberately chosen.
- Repo is public: https://github.com/Yuval-Peleg/remotica — README explicitly warns it's not ready for real printer use; MIT LICENSE backs that disclaimer.
- **Committing and pushing to this repo is pre-authorized** (confirmed 2026-07-31) — no need to ask before each commit/push, use judgment on when a chunk of work is worth committing and split into logical commits rather than one giant one.
