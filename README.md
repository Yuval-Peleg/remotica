# Remotica

A from-scratch, [OctoPrint](https://octoprint.org/)-inspired remote control dashboard for 3D printers.

It runs on the machine wired to your printer. You reach it from a browser on any other device on your network — no account, no cloud, no port forwarding.

> [!WARNING]
> **Don't leave this running an unattended print.** It drives heaters and motors, and parts of it are still unverified on real hardware. There is also **no login** — anyone on your network can control your printer. [Details below](#project-status).

---

## Install

Linux, x86_64 or arm64. On the machine wired to your printer:

```sh
curl -fsSL https://github.com/Yuval-Peleg/remotica/releases/latest/download/install.sh | sudo sh
```

Prebuilt binary, checksum-verified, installed as a systemd service. No toolchain, no Node, no repo checkout needed. It asks one question — start on boot? — and prints the address to open from your phone.

| Task | Command |
| --- | --- |
| Check it's running | `systemctl status remotica` |
| Watch the logs | `journalctl -u remotica -f` |
| Update | re-run the install command |
| Stop / start | `sudo systemctl stop remotica` / `start` |
| Uninstall | `sudo remotica-uninstall` |

Updating keeps your printer profile, uploaded G-code, and boot setting. Uninstalling keeps your data too — add `--purge` to delete it.

<details>
<summary><b>What it puts on the machine</b></summary>

| Path | What |
| --- | --- |
| `/usr/local/bin/remotica` | the binary |
| `/usr/local/bin/remotica-uninstall` | the uninstaller |
| `/usr/local/share/remotica/web/` | the dashboard |
| `/etc/systemd/system/remotica.service` | the service |
| `/etc/sudoers.d/remotica` | three `systemctl` commands, nothing else |
| `/etc/udev/rules.d/99-remotica-serial.rules` | USB serial access |
| `/var/lib/remotica/` | your profile and uploaded G-code |

Remotica runs as an unprivileged `remotica` user, not root. It needs elevated permission for exactly one thing — the start-on-boot toggle — so the sudoers file grants that user three specific `systemctl` command lines against this one service. A bug in Remotica can't become root.

</details>

## First run

1. Open the address the installer printed.
2. **Settings → confirm your printer.** Remotica may recognise it automatically, but you have to agree before controls unlock — a model name isn't a promise about bed size or temperature limits.
3. **Home the printer.** Movement stays blocked until it knows where the head is.
4. Upload G-code and print.

## Keeping the machine awake

Remotica blocks automatic suspend while a print is running — visible on the **System** page. This matters: if the host suspends, the printer *doesn't stop*. It holds its heaters at the last commanded temperature with nothing supervising it.

It deliberately does **not** override a human choosing Suspend, or a closing laptop lid. To disable sleep entirely:

```sh
# Desktop Ubuntu (GNOME), as the logged-in user:
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing'

# Any machine, headless included:
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

Laptop lid: set `HandleLidSwitch=ignore` in `/etc/systemd/logind.conf`, then `sudo systemctl restart systemd-logind`.

## Project status

Early, but real — it drives an actual printer. The caution is about how much has been *proven*, not whether it works.

| Confirmed on real hardware | Not yet |
| --- | --- |
| Connecting, port auto-discovery | Abort-safety on cancel (heaters off) |
| Live temperatures, setting targets | Recovery after an interrupted print |
| Jog, home, printing | Any access control |
| Webcam capture, hot-plug | Installer on a clean machine |

Two things to know before trusting it further:

- **No authentication.** Anyone who can reach the machine can drive your printer.
- **No resume.** If the backend dies or the machine loses power mid-print, Remotica starts fresh — it won't notice a print was in progress.

[`ROADMAP.md`](./ROADMAP.md) tracks the rest, roughly in priority order.

<details>
<summary><b>Disclaimer</b></summary>

Provided "as is", without warranty of any kind — see [LICENSE](./LICENSE). Because it interfaces directly with printer hardware, bugs could carry real physical risk, including fire or hardware damage. The author accepts no liability for damage, injury, or loss arising from its use, at any stage of development. **Use entirely at your own risk.**

</details>

## Running from source

For hacking on Remotica — use the installer above to actually run it.

```sh
sudo apt-get install build-essential   # plus Node
./run.sh          # real printer, auto-detected port
./run.sh --sim    # simulator, no hardware
```

`run.sh` builds the backend, starts both halves, and prints the LAN URL. It won't open a browser locally — that machine is meant to just sit there. Ctrl+C stops everything. For real hardware it offers a one-time `sudo` install of [`udev/99-remotica-serial.rules`](./udev/99-remotica-serial.rules) so serial access works without joining the `dialout` group.

## Tech stack

**Backend** — C, no framework, no runtime. Dependencies are vendored as source and compiled into the binary, so there's no package manager to keep happy on the printer PC:

- [civetweb](https://github.com/civetweb/civetweb) (MIT) — HTTP + WebSocket
- [cJSON](https://github.com/DaveGamble/cJSON) (MIT) — JSON
- POSIX `termios` (serial), V4L2 (webcam)

**Frontend** — React + Vite + Tailwind + [shadcn/ui](https://ui.shadcn.com/), dark-only sage-on-charcoal.

An installed Remotica is a **single process** — the backend serves the built frontend itself. In development the two are split, with Vite proxying the API.

## License

[MIT](./LICENSE).
