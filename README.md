# Remotica

A from-scratch, [OctoPrint](https://octoprint.org/)-inspired remote control dashboard for 3D printers.

## ⚠️ Project status: early work in progress

This project is **not ready for use** and should **not** be relied on to
control a real 3D printer at this stage. What's here so far is a frontend UI
(`frontend/`) working against simulated, local, in-browser data — there is no
backend, and nothing in this repo talks to real printer hardware yet.

## Disclaimer

This software is provided "as is", without warranty of any kind, express or
implied — see [LICENSE](./LICENSE). As this project grows a backend that
interfaces directly with printer hardware (heaters, motors, etc.), bugs in
that code could carry real physical risk, including but not limited to fire
or hardware damage. The author accepts no liability for damage, injury, or
loss arising from the use of this software, at any stage of its development.
**Use entirely at your own risk.**

## Tech stack

- `frontend/` — React + Vite + Tailwind CSS + [shadcn/ui](https://ui.shadcn.com/)
