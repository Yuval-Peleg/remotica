# Deployment: one-command install onto the printer PC

**Date:** 2026-08-08
**Status:** Approved, ready for planning

## Goal

Get Remotica onto a second Ubuntu machine — the one physically wired to the
printer — by downloading one thing from GitHub and running it. No C
toolchain, no Node, no `npm install`, no repo checkout on that machine.

Today this is impossible: `run.sh` builds the backend from source, runs
`npm install`, and starts a Vite dev server as a second process. That's a
development environment, not a deployment.

## Non-goals

Deliberately out of scope, each for a stated reason:

- **Authentication.** A real gap (`ROADMAP.md`), but its own project. The
  installer prints a warning that anyone on the LAN can drive the printer.
- **HTTPS.** LAN-only; the backend is compiled `NO_SSL` on purpose.
- **Auto-update.** Updating is re-running `install.sh`. Honest, and hard to
  get wrong.
- **Windows.** `ROADMAP.md` tracks it; the serial layer is the blocker, not
  packaging.
- **`remotica.local` / mDNS.** Attempted 2026-08-01 and hit a structural
  avahi limitation — see `ROADMAP.md`. The installer prints the IP.

## Architecture

Five parts, in dependency order. Part 1 gates everything else.

### Part 1 — Single-process serving

The backend serves the built frontend directly. This is the existing
"Single-process deployment" roadmap item, and nothing else in this spec
works until it lands.

**Static root.** Add `document_root` to civetweb's options array in
`main.c`, alongside the existing `listening_ports` / `num_threads` /
`request_timeout_ms`. Path comes from a new `--web-root <dir>` flag. API
routes are registered as explicit `mg_set_request_handler` handlers, which
civetweb matches before falling through to static files, so `/api/*` keeps
working with no ordering changes.

**SPA fallback — required, not optional.** `/settings` and `/about` exist
only in React Router. A browser that asks the *server* for `/settings`
directly — a refresh, a bookmark, a link shared to a phone — gets a 404
today. Needs a catch-all handler on `/` that serves `index.html` for any
path that is not under `/api/` and does not resolve to a real file under
the web root. Without this, the app appears broken on refresh, which is the
first thing anyone does.

**Data directory.** `main.c` hardcodes `PROFILE_PATH "data/profile.json"`
and `UPLOADS_DIR "data/uploads"`, both relative — the backend only works
when launched from `backend/`. A service running as an unprivileged user
from `/` cannot use those. Add `--data-dir <dir>`:

- Default when unset: `data` (current behaviour, so `run.sh` and every
  existing dev workflow are unchanged).
- The installed service passes `--data-dir /var/lib/remotica`.
- Profile and uploads paths are composed from it at startup rather than
  being compile-time constants.

**Version string.** No version exists anywhere today. Baked in at build
time via `-DREMOTICA_VERSION=\"$(git describe --tags)\"`, defaulting to
`"dev"` when the define is absent, so a plain `make` still compiles.

**Dev is unaffected.** `run.sh` passes neither new flag and keeps using the
Vite dev server with its proxy. Static serving is only exercised by an
installed build.

### Part 2 — Release artifacts

A GitHub Actions workflow triggered on a `v*` tag push. Per architecture
(`x86_64` and `arm64` — arm64 is nearly free in CI and means a Raspberry Pi
next to the printer works later with no redesign):

1. `npm ci && npm run build` in `frontend/`
2. `make` in `backend/`, with the version define set from the tag
3. Assemble and attach a tarball to the Release, plus a `SHA256SUMS` file

```
remotica-linux-<arch>.tar.gz
├── remotica                    the backend binary
├── web/                        contents of frontend/dist/
├── remotica.service            systemd unit
├── 99-remotica-serial.rules    copied from udev/
└── install.sh
```

`install.sh` is also attached to the Release as a standalone asset, so it
can be fetched and run before anything else is downloaded.

Installed layout on the target machine:

| Path | Contents |
|---|---|
| `/usr/local/bin/remotica` | binary |
| `/usr/local/bin/remotica-uninstall` | uninstaller |
| `/usr/local/share/remotica/web/` | frontend |
| `/etc/systemd/system/remotica.service` | unit |
| `/etc/sudoers.d/remotica` | three permitted commands |
| `/etc/udev/rules.d/99-remotica-serial.rules` | USB-serial access |
| `/var/lib/remotica/` | profile + uploaded G-code (owned by `remotica`) |

The unit itself runs the same command a person would:

```ini
[Service]
User=remotica
ExecStart=/usr/local/bin/remotica --serial auto \
          --web-root /usr/local/share/remotica/web \
          --data-dir /var/lib/remotica
Restart=on-failure
RestartSec=5
```

`--serial auto` rather than a fixed device path, so the printer reconnects
after a replug regardless of whether it comes back as the same
`/dev/ttyACM*`. `Restart=on-failure` covers a crash; it does not paper over
a missing printer, since the backend already starts fine with none attached
and retries in the background.

### Part 3 — The installer

Run as `curl -fsSL <release>/install.sh | sudo sh`. Needs root: it writes
to `/usr/local`, `/etc`, and creates a user.

Steps, in order, each reporting what it did:

1. Detect architecture via `uname -m`; refuse clearly on anything other
   than `x86_64` / `aarch64` rather than downloading a binary that can't run.
2. Download the matching tarball and `SHA256SUMS`; **verify the checksum
   before extracting anything.** Abort on mismatch.
3. Create the service user: `useradd --system --no-create-home --shell
   /usr/sbin/nologin remotica`. Idempotent — skip if it exists.
4. Install the files per the table above.
5. Install the udev rule and run `udevadm control --reload-rules &&
   udevadm trigger`, so USB-serial works without `dialout` membership.
   Reuses the existing rule file verbatim.
6. Install `/etc/sudoers.d/remotica`, validated with `visudo -c -f` before
   being moved into place — a malformed sudoers file can lock out `sudo`
   machine-wide, so it is never written directly.
7. Ask the boot question, if and only if stdin is a terminal:
   `Start Remotica automatically when this PC boots? [Y/n]`
8. `systemctl daemon-reload`, enable if answered yes, and start.
9. Print the LAN URL (`hostname -I`, first address, port 8080) and the
   no-authentication warning.

**`--no-interactive`** skips step 7 and defaults boot-start to *off*. Same
reasoning as `run.sh`'s existing udev prompt: a script piped into `sh` with
no terminal has nobody to answer a `read`, and must not hang. Defaulting to
off rather than on means an unattended install never silently arranges for
something to auto-start.

Re-running the installer is the upgrade path: it must be idempotent, and it
must preserve `/var/lib/remotica` and the current boot-start setting rather
than resetting them.

The sudoers file, in full:

```
remotica ALL=(root) NOPASSWD: /usr/bin/systemctl enable remotica.service, \
                              /usr/bin/systemctl disable remotica.service, \
                              /usr/bin/systemctl is-enabled remotica.service
```

Three exact command lines, absolute paths, no wildcards, no argument
freedom. The backend runs as an unprivileged user; a memory-safety bug in
civetweb, the JSON parser, or the serial reader does not become root.

### Part 4 — The uninstaller

Installed as `/usr/local/bin/remotica-uninstall`, generated from the same
source as the installer so the two cannot drift apart on what exists.

Removes, in this order: stop the service, disable it, delete the unit file,
`systemctl daemon-reload`, delete the sudoers file, delete the udev rule and
reload udev, delete `/usr/local/share/remotica/`, delete the `remotica`
user, delete both binaries (itself last).

**It does not delete `/var/lib/remotica` by default.** That directory holds
the printer profile and every uploaded G-code file. It prints what it left
and the one command to remove it. `--purge` removes it too, after an
explicit confirmation when a terminal is attached.

Anything already absent is skipped without failing, so a partially-completed
install can still be cleaned up.

### Part 5 — The System page

`frontend/src/pages/System` is a placeholder today. It becomes the page that
answers "is this thing healthy, and will it come back after a power cut?"

**`GET /api/system`** returns:

```json
{
  "version": "v0.1.0",
  "uptimeSeconds": 4210,
  "dataDir": "/var/lib/remotica",
  "dataFreeBytes": 41231237120,
  "serialPort": "/dev/ttyACM0",
  "managed": true,
  "bootStartEnabled": false
}
```

`managed` is the important field. It is true only when running as an
installed systemd service — detected by the presence of the unit file plus
a successful `systemctl is-enabled`. In a dev checkout there is no service
to toggle, so `managed` is false, `bootStartEnabled` is omitted, and the UI
renders the switch disabled with an explanation rather than a control that
silently fails.

**`POST /api/system/boot-start`** with `{"enabled": true|false}` runs the
corresponding permitted `sudo systemctl` command. Returns 409 when
`managed` is false. Returns 500 with the command's stderr on failure —
surfacing the real error, because a sudoers misconfiguration is otherwise
invisible from the UI.

Frontend: a real `System.jsx` showing version, uptime, data directory and
free space, serial port, and the boot-start switch. Follows the existing
card/`motion-stagger` conventions. Errors surface as toasts, matching
`use-printer-state.js`.

## Error handling

- **Checksum mismatch** — abort before extracting; never install an
  unverified binary.
- **Unsupported architecture** — refuse with the detected `uname -m` named
  explicitly.
- **`visudo -c` fails** — abort without moving the file into `/etc/sudoers.d/`.
- **Service fails to start** — installer prints `journalctl -u remotica -n
  50` output rather than only "failed".
- **`sudo systemctl` fails from the backend** — returned to the UI as a
  toast carrying stderr, not swallowed.
- **Missing web root** — the backend logs a clear warning at startup and
  still serves the API, so a broken frontend install doesn't take the
  printer control with it.

## Verification

1. `make` in a clean checkout with no version define — still builds, still
   runs from `backend/` with relative `data/`.
2. `./run.sh --sim` unchanged: Vite dev server, proxy, all routes work.
3. Built binary with `--web-root`/`--data-dir`: dashboard loads, and a hard
   refresh on `/settings` and `/about` returns the app, not a 404. This is
   the check that catches a missing SPA fallback.
4. Full install on a clean Ubuntu VM (not the dev machine): install, answer
   yes to boot-start, reboot, confirm the dashboard is reachable from
   another device with nobody having logged in.
5. Toggle boot-start from the System page; confirm with `systemctl
   is-enabled remotica` on the box.
6. Upload a G-code file, then re-run `install.sh`: file and profile survive.
7. `remotica-uninstall`, then confirm every path in the layout table is
   gone, `/var/lib/remotica` is not, `sudo` still works, and `systemctl
   status remotica` reports no unit.
8. `remotica-uninstall --purge` removes the data directory too.

## Documentation

`README.md` gains an **Installation** section as the primary path for
getting Remotica running on a printer PC — the one-line install command,
what it puts on the machine, how to check on it (`systemctl status`,
`journalctl -u remotica -f`), how to update, and how to uninstall including
the data-directory caveat. The existing "Running it" section stays, retitled
to make clear it is the from-source development path, not the way to deploy.

`ROADMAP.md`: the "Single-process deployment" item is resolved and removed.

`CLAUDE.md`: "Deployment model" stops saying "not built yet"; the new flags,
the release workflow, and the installer/uninstaller get documented.
`frontend/CLAUDE.md`: System is no longer a placeholder.

## Critical files

- `backend/src/main.c` — `document_root`, SPA fallback, `--web-root`,
  `--data-dir`, version define
- `backend/src/api_handlers.h/.c` — `/api/system`, `/api/system/boot-start`
- `backend/Makefile` — version define
- `packaging/install.sh`, `packaging/remotica.service` — new
- `.github/workflows/release.yml` — new
- `frontend/src/pages/System.jsx` — new (replaces the placeholder)
- `frontend/src/lib/api.js`, `frontend/src/App.jsx`
- `README.md`, `ROADMAP.md`, `CLAUDE.md`, `frontend/CLAUDE.md`
