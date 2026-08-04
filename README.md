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

## Running it

Linux only for now (headless-friendly — no desktop environment needed). You
need a C toolchain and Node:

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

Frontend and backend are meant to end up as a single process, with the
backend serving the built frontend directly. That isn't wired up yet — today
`run.sh` starts them as two processes with a dev proxy in between. See
[`ROADMAP.md`](./ROADMAP.md).

## License

[MIT](./LICENSE).
