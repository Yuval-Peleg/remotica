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

## Windows support

Planned for later. Only the serial layer (POSIX `termios` → Win32 serial
API) should need to change — civetweb and cJSON are already cross-platform.
