# Roadmap

Known gaps and planned work, roughly in the order they matter for getting
Remotica ready for real, unattended printer use. This is a living list, not
a schedule — see `CLAUDE.md` for how each already-built piece works today.

## Real-hardware verification still outstanding

**A full print is done (2026-08-08):** a 5-hour job completed start to
finish on a real Ender 3, via an installed v0.1.2. That closes the
headline milestone below — print streaming, the checksum/resend path
under sustained load, jog, home and temperature-set are all exercised by
that run. What remains outstanding is the *abort* path: nobody has yet
watched a cancel from the UI actually drop the heaters on real hardware,
which is the one failure that already bit once (see below).


`transport_serial.c` has only been partially verified against a real
printer (an Ender 3, stock Marlin 2.1.2.4, 2026-08-01) — connect,
`--serial auto` discovery, and temperature polling are confirmed working.
**Jog, home, temperature-set commands, and actual print streaming (the
checksum/resend path) still haven't been run against real hardware.** This
matters most, since a bug here could crash a print or drive a heater/motor
incorrectly.

A first real print attempt on the same day got far enough to expose two
bugs and then stopped: a false abort on a legitimately-slow move (busy
keepalives weren't extending the command deadline) and — the serious one —
an abort-safety sequence that silently no-op'd, leaving the hotend and bed
at full temperature for 20+ minutes while the backend reported them shut
down. Both are fixed and now covered by `backend/tools/fake_marlin_test.py`
(scenarios C and D), but **the fixes have only been verified against the
pty harness, not re-run against the physical printer.** A full real print,
start to finish, is still the outstanding milestone — and specifically
worth watching: whether the abort-safety sequence actually drops the
heaters when a print is cancelled from the UI.

## Camera: multiple cameras, and reconnect while streaming

Frame capture itself is verified as of 2026-08-08 (real UVC webcam, real
640x480 JPEG frames over the MJPEG stream), and the camera is now
hot-pluggable — plugging one in or replugging it is picked up without a
restart or a page refresh.

Two gaps remain. **Only the first usable `/dev/video*` device is ever
used**: a machine with two cameras can't choose between them, and
there's no UI for it. And **an unplug is only noticed once capture is
actually running** — the capture thread's read failure is what detects
it. A camera unplugged while nobody is viewing the stream still shows as
available until someone opens it, since noticing sooner would mean
polling the device, which lights its privacy LED.

## Installer not yet verified on a clean machine

The single-process build, the systemd unit, the installer and the
uninstaller are all written and their pieces individually checked (the
packaged payload runs and serves the dashboard; the sudoers file passes
`visudo -c`; the release tarball assembles and verifies) — but the
**install has never been run end to end on a clean machine**, because
doing that on the development machine would drop a service and a sudoers
file over the working setup. Still outstanding: install on a fresh
Ubuntu box, reboot, confirm the dashboard comes back with nobody logged
in, toggle start-on-boot from the System page (the only real test of the
sudoers rules), then uninstall and confirm nothing is left behind and
`sudo` still works.

## Host power management beyond idle suspend

**The inhibitor does not reliably work, and why is unknown (2026-08-08).**
A machine running v0.2.0 suspended mid-print anyway. `journalctl -u
remotica` contains none of power_inhibit.c's log lines — neither the
"Blocking automatic suspend" success nor the "systemd-inhibit not found"
fallback — even though journald is capturing the service's stdout, the
call site is present in the shipped binary, and it sits in the tick loop
guarded only by the job status. So the code appears never to have run,
which is not yet explained. Next diagnostic: the System page's Sleep row
during a print, which reports the same flag without needing the journal.
Until that's understood, the installer offers to mask the sleep targets
outright, which is what actually protects a print. Deliberately **not** covered: a human
choosing Suspend, closing a laptop lid, or the machine losing power.
Blocking those would mean overriding the owner's explicit intent, so the
README documents how to disable them instead.

Also unhandled: what happens *after* an interruption. There's no
resume-after-power-loss, and no detection on startup that a print was in
progress when the backend last died — the job state simply starts fresh.

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
