# Roadmap

Known gaps and planned work, roughly in the order they matter for getting
Remotica ready for real, unattended printer use. This is a living list, not
a schedule — see `CLAUDE.md` for how each already-built piece works today.

## Real-hardware verification still outstanding

`transport_serial.c` has only been partially verified against a real
printer (an Ender 3, stock Marlin 2.1.2.4, 2026-08-01) — connect,
`--serial auto` discovery, and temperature polling are confirmed working.
**Jog, home, temperature-set commands, and actual print streaming (the
checksum/resend path) still haven't been run against real hardware.** This
matters most, since a bug here could crash a print or drive a heater/motor
incorrectly.

## Camera capture verification

`camera.c`'s device discovery is verified against a real webcam, but the
actual mmap-based V4L2 capture loop and MJPEG streaming
(`camera_stream_handler`) have not been checked against real frame
capture — see the warning in `camera.h`.

## Single-process deployment

Frontend and backend still run as two separate processes (`run.sh`'s Vite
dev server + the backend, talking over a dev proxy) instead of the backend
serving the built `frontend/dist/` directly, which is the intended final
deployment model (see `CLAUDE.md`'s "Deployment model" section). Needs
civetweb's static-file serving wired up to a production frontend build.

## Detecting a printer going away mid-session

The reconnect thread added 2026-08-01 covers "no printer at startup, plug
one in later" — but nothing currently notices a printer that was connected
going away mid-session (e.g. the USB cable is physically unplugged, or the
printer loses power) and reflects that back as disconnected. `state->connected`
is only ever set by `serial_connect()`/`serial_disconnect()` today; a
failed mid-session read/write (e.g. during the M105 temperature poll) is
silently ignored rather than tripping a reconnect.

## No authentication on the LAN

Once frontend and backend are reachable from other devices on the LAN
(see `run.sh`'s `--host` binding), anyone on the network with the URL can
control the printer — there's no login, no access control at all. Fine for
a trusted home network, not fine beyond that.

## Live position tracking during homing

Right now homing just grays out the bed schematic with a "Homing…" overlay
while it's in progress, since the backend doesn't poll position during a
blocking `G28` (see `BedSchematic.jsx`/`ControlPanel.jsx`). Real live
tracking during homing would need the backend to poll position (e.g. `M114`)
during the home command — not implemented.

## 250000 baud support

Some newer 32-bit printer mainboards default to 250000 baud instead of
115200. There's no standard POSIX `Bxxxx` constant for it, so supporting it
needs Linux's termios2/`BOTHER` ioctl mechanism for arbitrary custom rates
— not implemented (`BAUD_RATE_OPTIONS` in `transport_serial.c` only lists
115200).

## A friendly LAN URL instead of an IP:port

Right now reaching Remotica from another device on the LAN means typing an
IP and port (`run.sh` prints it — see `--host` above). A `remotica.local`
via mDNS/avahi was attempted 2026-08-01 and hit a real dead end worth
recording so it isn't retried blind:

Adding a static alias for the *same* IP a machine already owns under its
own primary hostname (`/etc/avahi/hosts`, `<ip> remotica.local` alongside
avahi's own dynamic `<real-hostname>.local` record for that same IP) fails
with `avahi_server_add_address failure: Local name collision` — confirmed
via `journalctl -u avahi-daemon`, and confirmed NOT caused by another
device actually squatting the name (a raw mDNS probe for `remotica.local`
got zero real responses). This isn't a config mistake, it's structural:
`/etc/avahi/hosts` is meant for publishing names on behalf of a device
that *doesn't* run avahi itself (see the file's own header comment) — using
it to give an already-avahi-enabled host a *second* name trips avahi's own
conflict-probe logic. Neither a `reload` (SIGHUP) nor a full
`systemctl restart avahi-daemon` fixes it.

What would actually work, for a future pass:
- **Rename the machine's real hostname** to `remotica` (`hostnamectl`) —
  avoids the two-names-one-IP collision entirely since there'd only be one
  name. More invasive (shell prompt, SSH host identity) and per-machine,
  not something an install wizard can safely do unattended.
- Investigate whether avahi's client-side publish API
  (`avahi_entry_group_add_address` via a small persistent helper process,
  vs. the bulk static-hosts-file loader used above) sidesteps the same
  collision check, or hits it too — untested.
- Punt entirely and rely on router-level DNS (some routers let you assign
  a local DNS name to a device by IP/MAC) — outside this project's
  control, but worth documenting as a per-user option in an eventual
  install wizard rather than something Remotica sets up itself.

## Windows support

Planned for later. Only the serial layer (POSIX `termios` → Win32 serial
API) should need to change — civetweb and cJSON are already cross-platform.
