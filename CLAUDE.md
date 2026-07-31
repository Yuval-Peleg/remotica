# Remotica

An from-scratch, OctoPrint-inspired remote control dashboard for 3D printers. Dark/sage-green theme throughout. Not ready for real printer use yet — see README.md.

## Repo layout

- `frontend/` — the active app. See `frontend/CLAUDE.md` for details.
- `backend/` — started. C, using civetweb (embedded HTTP + WebSocket server) and cJSON, both vendored under `backend/third_party/`. See "Planned backend" below.
- `legacy-static/`, `images/` — old hand-built mockup, kept on disk for reference only. Gitignored on purpose — do not re-add to git without asking.

## Deployment model

Frontend and backend run as a **single process** on the same PC that's physically wired to the printer (same model OctoPrint uses). The backend serves the already-built frontend directly — no separate hosting, no CORS setup. Other devices reach it over LAN by browsing to that PC's IP. No accounts, no cloud relay, no port-forwarding.

## Planned backend

- **Language:** C — deliberate choice, not a placeholder.
- **HTTP + WebSocket:** [civetweb](https://github.com/civetweb/civetweb) (MIT), one embeddable library for both. **Not Mongoose** — Mongoose looked like the obvious pick early on, but it's dual-licensed GPLv2/commercial with no permissive option, which would force this MIT-licensed repo to either go GPL or pay for a commercial license. civetweb is a permissively-licensed sibling (same original codebase lineage) with the same HTTP+WebSocket feature set, so it was swapped in instead. If any other C dependency gets added later, check its license before vendoring it — same issue could recur.
- **JSON:** [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Both vendored as source under `backend/third_party/` (civetweb 1.16, cJSON 1.7.19) — no package manager, just `.c`/`.h` files compiled straight into the binary. Their upstream LICENSE files are kept alongside them.
- **API shape:** REST for one-off actions (upload gcode, start/pause/cancel print, jog, set temps, get/set printer profile). WebSocket for continuous push of live state (temps, position, progress, logs) to every connected browser. So far only a `POST /api/command` smoke-test route exists (`backend/src/main.c`) — it parses whatever JSON body it's sent, prints it to stdout, and echoes it back. It's not a real command yet, just proof the frontend can reach the backend.
- **Build:** `cd backend && make` → `backend/build/remotica-backend`. Built with `NO_SSL` (no HTTPS needed on LAN) and `USE_WEBSOCKET` defined. Needs a C toolchain (`gcc`/`make`) — not installed on this machine yet, install via `sudo apt-get install build-essential`.
- **Target OS:** Linux first, headless-compatible (Ubuntu Server, no GUI needed — none of the planned libraries touch a display). A Windows fork is planned for later; only the serial layer (POSIX `termios` → Win32 serial API) should need to change, since civetweb/cJSON are already cross-platform.
- **Printer link:** USB serial, G-code in/out, parsed printer replies (temperature reports, `ok`, errors/resend). Not built yet.
- **Frontend dev proxy:** `frontend/vite.config.js` proxies `/api` → `http://localhost:8080` during `npm run dev`, so the frontend can call relative `/api/...` URLs without CORS setup. In production the backend serves the built frontend itself (same origin), so the proxy isn't needed there.
- Once real backend state exists, the frontend's mock hooks (`use-printer-temps.js`, local jog state in `ControlPanel.jsx`, printer profile in `printer-profile.js`) need to flip from generating fake state to syncing from the backend. `frontend/src/components/dashboard/BackendConnectionTest.jsx` is a **temporary** dev-only panel for testing the connection — remove it once real controls talk to the backend for real.

## Formatting / static analysis

- **C:** `.clang-format` at repo root (LLVM base, 4-space indent, 100 col). Run `clang-format -i backend/src/*.c` (don't run it on `backend/third_party/` — that's vendored upstream code, leave it as-is). Use `cppcheck backend/src/` for static analysis. None of `clang-format`/`cppcheck`/a C toolchain are installed yet — install via `sudo apt-get install build-essential clang-format cppcheck`.
- **Frontend:** see `frontend/CLAUDE.md`.

## Git

- Only `frontend/` (and this file) are tracked. `legacy-static/` and `images/` are gitignored on purpose.
- Git identity is already configured (`Yuval Peleg`, a GitHub noreply email) — **never run `git config` to change it**, that email was deliberately chosen.
- Repo is public: https://github.com/Yuval-Peleg/remotica — README explicitly warns it's not ready for real printer use; MIT LICENSE backs that disclaimer.
- **Committing and pushing to this repo is pre-authorized** (confirmed 2026-07-31) — no need to ask before each commit/push, use judgment on when a chunk of work is worth committing and split into logical commits rather than one giant one.
