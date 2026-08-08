# Remotica

A from-scratch, [OctoPrint](https://octoprint.org/)-inspired remote control
dashboard for 3D printers.

Remotica runs on the machine that's physically wired to your printer. You
reach it from a browser on any other device on the same network — no
account, no cloud relay, no port forwarding.

## ⚠️ Project status: early work in progress

This project is **not ready to be trusted with a real print** and should not
be left running one unattended.

The pieces are real — there's a C backend that talks G-code to a printer
over USB, streams a print line by line, and serves live state to the
frontend over a WebSocket — but the code that drives heaters and motors has
only been **partly** verified against physical hardware. Connecting, port
discovery, and temperature polling are confirmed working against a real
Ender 3 (stock Marlin 2.1.2.4). Jog, home, temperature-set, and a full print
from start to finish are **not** yet confirmed on real hardware; they've
only been exercised against a pty-based fake firmware
(`backend/tools/fake_marlin_test.py`).

There is also **no authentication of any kind**. Anyone who can reach the
machine on your network can drive your printer.

Treat this as something to experiment with while you're standing next to the
printer. [`ROADMAP.md`](./ROADMAP.md) tracks what's missing, roughly in the
order it matters.

## Disclaimer

This software is provided "as is", without warranty of any kind, express or
implied — see [LICENSE](./LICENSE). Because it interfaces directly with
printer hardware (heaters, motors, etc.), bugs in it could carry real
physical risk, including but not limited to fire or hardware damage. The
author accepts no liability for damage, injury, or loss arising from the use
of this software, at any stage of its development. **Use entirely at your
own risk.**

## Installation

Linux only for now, and headless-friendly — the machine wired to your
printer needs no desktop environment, and you never have to sit in front
of it.

On that machine:

```sh
curl -fsSL https://github.com/Yuval-Peleg/remotica/releases/latest/download/install.sh | sudo sh
```

That downloads a prebuilt binary (x86_64 or arm64), verifies its
checksum, installs it as a systemd service, and prints the address to
open from your phone or laptop. There's no toolchain to install, no Node,
and no copy of this repository on that machine.

It asks one question — whether Remotica should start automatically when
the machine boots — and you can change your mind later from the
dashboard's **System** page.

What it puts on the machine:

| Path | What it is |
| --- | --- |
| `/usr/local/bin/remotica` | the binary |
| `/usr/local/bin/remotica-uninstall` | the uninstaller |
| `/usr/local/share/remotica/web/` | the dashboard |
| `/etc/systemd/system/remotica.service` | the service |
| `/etc/sudoers.d/remotica` | see below |
| `/etc/udev/rules.d/99-remotica-serial.rules` | USB serial access |
| `/var/lib/remotica/` | your printer profile and uploaded G-code |

Remotica runs as its own unprivileged `remotica` user, not as root. The
one thing it needs elevated permission for is the start-on-boot toggle,
so the sudoers file grants that user exactly three `systemctl` command
lines against exactly this service — nothing else. A bug in Remotica
can't become root.

**Checking on it:**

```sh
systemctl status remotica      # is it running?
journalctl -u remotica -f      # follow the logs
```

**Updating:** re-run the install command. Your printer profile and
uploaded files are kept, as is your start-on-boot setting.

**Uninstalling:**

```sh
sudo remotica-uninstall            # removes everything except your data
sudo remotica-uninstall --purge    # also deletes /var/lib/remotica
```

Plain `remotica-uninstall` deliberately leaves `/var/lib/remotica` alone
— that's your printer profile and every G-code file you've uploaded — and
tells you where it is.

> **Anyone on your network who opens the dashboard can control your
> printer.** Remotica has no login of any kind. Only run it on a network
> you trust.

### Keeping the machine awake

Remotica blocks automatic suspend for as long as a print is running, so a
machine nobody has touched for hours won't doze off mid-print. You can
see this on the dashboard's **System** page while printing.

This matters more than a ruined print: if the host suspends, the printer
doesn't stop — it holds its heaters at the last commanded temperature
with nothing supervising it until someone wakes the machine.

What Remotica does **not** override, deliberately, is a human choosing to
suspend, or a laptop lid being closed. If you want the machine never to
sleep at all:

```sh
# Desktop Ubuntu (GNOME), as the logged-in user:
gsettings set org.gnome.settings-daemon.plugins.power   sleep-inactive-ac-type 'nothing'

# Any machine, including headless — disables suspend entirely:
sudo systemctl mask sleep.target suspend.target   hibernate.target hybrid-sleep.target
```

A laptop being used as the printer PC also needs its lid-close behaviour
changed — set `HandleLidSwitch=ignore` in `/etc/systemd/logind.conf` and
`sudo systemctl restart systemd-logind`.

## Running from source (development)

This is the development path — for hacking on Remotica, not for putting
it on the machine next to your printer. Use the installer above for that.

You need a C toolchain and Node:

```sh
sudo apt-get install build-essential
```

Then, from the repo root:

```sh
./run.sh          # real printer, auto-detecting the serial port
./run.sh --sim    # built-in simulator, no hardware needed
```

`run.sh` builds the backend if needed, starts both halves, waits until
they're actually reachable, and prints the LAN URL to open from your phone
or laptop. It deliberately does not open a browser on the machine it runs
on — that machine is meant to just sit there wired to the printer. Ctrl+C
stops both processes.

For real hardware it also offers a one-time, `sudo`-gated install of
[`udev/99-remotica-serial.rules`](./udev/99-remotica-serial.rules), so USB
serial access works without adding yourself to the `dialout` group and
logging back in. That file's header explains exactly what it does and how to
install it by hand instead.

## Tech stack

**Backend** — C, no framework and no runtime. Both dependencies are vendored
as source under `backend/third_party/` and compiled straight into the
binary, so there's no package manager to keep happy on the machine that's
supposed to just sit there and run:

- [civetweb](https://github.com/civetweb/civetweb) (MIT) — embedded HTTP +
  WebSocket server
- [cJSON](https://github.com/DaveGamble/cJSON) (MIT) — JSON
- POSIX `termios` for the serial link, V4L2 for the optional webcam

**Frontend** — React + Vite + Tailwind CSS +
[shadcn/ui](https://ui.shadcn.com/), dark-only sage-on-charcoal theme.

An installed Remotica is a **single process**: the backend serves the
built frontend itself, so there's nothing else to run and no web server
to configure. During development the two halves are still split, with
Vite serving the frontend for hot reload and proxying the API — that's
what `run.sh` starts.

## License

[MIT](./LICENSE).
