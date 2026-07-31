#!/usr/bin/env bash
#
# run.sh
# =======
# One command to get Remotica fully running for a test session: builds
# the backend if needed, starts it, starts the frontend dev server,
# waits for both to actually be reachable, reports whether a printer was
# found, and opens the dashboard in a browser. Ctrl+C stops everything
# together.
#
# Usage:
#   ./run.sh          # real printer, auto-detected (--serial auto)
#   ./run.sh --sim    # simulator instead — no hardware needed
#
# This deliberately still runs the frontend as a separate Vite dev
# server (not served by the backend as one process) — that's a real,
# documented gap (see root CLAUDE.md's "Not done yet"), but wiring up
# static file serving is its own piece of backend work, and this script
# exists specifically so you don't have to touch backend code right
# before a hardware test session.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$SCRIPT_DIR/backend"
FRONTEND_DIR="$SCRIPT_DIR/frontend"
BACKEND_LOG="$BACKEND_DIR/backend.log"
FRONTEND_LOG="$FRONTEND_DIR/vite.log"
BACKEND_URL="http://localhost:8080"
FRONTEND_URL="http://localhost:5173"

# --sim swaps out --serial auto for no flag at all, which main.c already
# treats as "use the simulator" — see backend/src/main.c.
SERIAL_ARGS=(--serial auto)
if [[ "${1:-}" == "--sim" ]]; then
    SERIAL_ARGS=()
fi

BACKEND_PID=""
FRONTEND_PID=""
TAIL_PID=""

# Runs on Ctrl+C AND on normal exit (bash fires the EXIT trap either
# way) — killing an already-dead PID with 2>/dev/null is harmless, so
# this is safe to run twice if both traps happen to fire.
cleanup() {
    echo
    echo "Shutting down..."
    [[ -n "$TAIL_PID" ]] && kill "$TAIL_PID" 2>/dev/null
    [[ -n "$FRONTEND_PID" ]] && kill "$FRONTEND_PID" 2>/dev/null
    [[ -n "$BACKEND_PID" ]] && kill "$BACKEND_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

echo "=== Building the backend ==="
if ! (cd "$BACKEND_DIR" && make); then
    echo "Backend build failed — see the error above."
    exit 1
fi

if [[ ! -d "$FRONTEND_DIR/node_modules" ]]; then
    echo
    echo "=== Installing frontend dependencies (first run only) ==="
    if ! (cd "$FRONTEND_DIR" && npm install); then
        echo "npm install failed — see the error above."
        exit 1
    fi
fi

echo
if [[ "${#SERIAL_ARGS[@]}" -eq 0 ]]; then
    echo "=== Starting backend (simulator) ==="
else
    echo "=== Starting backend (--serial auto — scanning for a real printer) ==="
    echo "Hand near the power switch, watch the printer once it connects."
fi

(cd "$BACKEND_DIR" && exec ./build/remotica-backend "${SERIAL_ARGS[@]}") >"$BACKEND_LOG" 2>&1 &
BACKEND_PID=$!

# Streams the backend's log live to this terminal (its own stdout was
# redirected to the log file above, not silenced) — this is how you see
# "Found a printer at ..." / "No printer found" without the script
# having to parse and re-print every line itself.
tail -n +1 -f "$BACKEND_LOG" &
TAIL_PID=$!

echo "Waiting for the backend to come up..."
backend_ready=0
for _ in $(seq 1 60); do
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        echo
        echo "Backend exited before starting up — see the log above."
        if [[ "${#SERIAL_ARGS[@]}" -gt 0 ]]; then
            echo "(Most likely: no printer was found — check it's plugged in and powered on.)"
        fi
        exit 1
    fi
    if curl -s -o /dev/null "$BACKEND_URL/api/state"; then
        backend_ready=1
        break
    fi
    sleep 1
done

if [[ "$backend_ready" -ne 1 ]]; then
    echo "Backend never became reachable after 60s — see the log above."
    exit 1
fi

curl -s "$BACKEND_URL/api/state" | python3 -c "
import json, sys
d = json.load(sys.stdin)
print()
print('=== Printer status ===')
print('Connected:', d['connected'])
print('Connection type:', d['connectionType'])
if d.get('firmwareInfo'):
    print('Firmware hint:', d['firmwareInfo'])
print()
"

echo "=== Starting frontend ==="
# Execs directly into vite's own binary rather than `npm run dev` —
# npm doesn't exec into its child, it forks one, so $! from `npm run
# dev &` would capture npm's PID while the actual vite/node process
# keeps running as an orphan after npm is killed. Execing straight into
# vite (same trick already used for the backend above) means $! IS the
# real process, so killing FRONTEND_PID in cleanup() actually works.
(cd "$FRONTEND_DIR" && exec ./node_modules/.bin/vite) >"$FRONTEND_LOG" 2>&1 &
FRONTEND_PID=$!

echo "Waiting for the frontend to come up..."
for _ in $(seq 1 30); do
    if curl -s -o /dev/null "$FRONTEND_URL"; then
        break
    fi
    sleep 1
done

echo
echo "Remotica is running: $FRONTEND_URL"

if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$FRONTEND_URL" >/dev/null 2>&1 &
elif command -v firefox >/dev/null 2>&1; then
    firefox "$FRONTEND_URL" >/dev/null 2>&1 &
else
    echo "(Couldn't find a way to open a browser automatically — open the URL above yourself.)"
fi

echo
echo "Press Ctrl+C to stop everything."
wait "$BACKEND_PID" "$FRONTEND_PID"
