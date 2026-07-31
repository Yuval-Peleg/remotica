# Remotica

An from-scratch, OctoPrint-inspired remote control dashboard for 3D printers. Dark/sage-green theme throughout. Not ready for real printer use yet — see README.md.

## Repo layout

- `frontend/` — the active app. See `frontend/CLAUDE.md` for details.
- `backend/` — not started yet. Planned: C, using Mongoose (embedded HTTP + WebSocket server) and cJSON. See "Planned backend" below.
- `legacy-static/`, `images/` — old hand-built mockup, kept on disk for reference only. Gitignored on purpose — do not re-add to git without asking.

## Deployment model

Frontend and backend run as a **single process** on the same PC that's physically wired to the printer (same model OctoPrint uses). The backend serves the already-built frontend directly — no separate hosting, no CORS setup. Other devices reach it over LAN by browsing to that PC's IP. No accounts, no cloud relay, no port-forwarding.

## Planned backend

- **Language:** C — deliberate choice, not a placeholder.
- **HTTP + WebSocket:** [Mongoose](https://mongoose.ws/), one embeddable library for both.
- **JSON:** [cJSON](https://github.com/DaveGamble/cJSON).
- **API shape:** REST for one-off actions (upload gcode, start/pause/cancel print, jog, set temps, get/set printer profile). WebSocket for continuous push of live state (temps, position, progress, logs) to every connected browser.
- **Target OS:** Linux first, headless-compatible (Ubuntu Server, no GUI needed — none of the planned libraries touch a display). A Windows fork is planned for later; only the serial layer (POSIX `termios` → Win32 serial API) should need to change, since Mongoose/cJSON are already cross-platform.
- **Printer link:** USB serial, G-code in/out, parsed printer replies (temperature reports, `ok`, errors/resend).
- Once this exists, the frontend's mock hooks (`use-printer-temps.js`, local jog state in `ControlPanel.jsx`, printer profile in `printer-profile.js`) need to flip from generating fake state to syncing from the backend.

## Formatting / static analysis

- **C** (once `backend/` exists): `.clang-format` at repo root (LLVM base, 4-space indent, 100 col). Run `clang-format -i <files>`. Use `cppcheck` for static analysis. Neither is installed yet — install via `sudo apt-get install clang-format cppcheck`.
- **Frontend:** see `frontend/CLAUDE.md`.

## Git

- Only `frontend/` (and this file) are tracked. `legacy-static/` and `images/` are gitignored on purpose.
- Git identity is already configured (`Yuval Peleg`, a GitHub noreply email) — **never run `git config` to change it**, that email was deliberately chosen.
- Repo is public: https://github.com/Yuval-Peleg/remotica — README explicitly warns it's not ready for real printer use; MIT LICENSE backs that disclaimer.
- **Committing and pushing to this repo is pre-authorized** (confirmed 2026-07-31) — no need to ask before each commit/push, use judgment on when a chunk of work is worth committing and split into logical commits rather than one giant one.
