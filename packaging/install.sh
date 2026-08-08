#!/bin/sh
#
# install.sh
# ==========
# Installs Remotica onto the machine that's physically wired to your
# printer, as a systemd service running under its own unprivileged user.
#
#   curl -fsSL https://github.com/Yuval-Peleg/remotica/releases/latest/download/install.sh | sudo sh
#
# The same script is installed as /usr/local/bin/remotica-uninstall and
# removes everything again when invoked under that name. One file rather
# than two so the install and uninstall lists cannot drift apart — the
# usual way an uninstaller ends up leaving things behind.
#
# Flags:
#   --uninstall        remove everything (implied when run as remotica-uninstall)
#   --purge            with --uninstall, also delete /var/lib/remotica
#   --no-interactive   never prompt; boot-start defaults to off
#
# Deliberately POSIX sh, not bash: this has to run on a minimal server
# install, and there's nothing here that needs more than sh.

set -eu

REPO="Yuval-Peleg/remotica"
SERVICE_USER="remotica"
UNIT_NAME="remotica.service"

BIN_PATH="/usr/local/bin/remotica"
UNINSTALL_PATH="/usr/local/bin/remotica-uninstall"
SHARE_DIR="/usr/local/share/remotica"
DATA_DIR="/var/lib/remotica"
UNIT_PATH="/etc/systemd/system/$UNIT_NAME"
SUDOERS_PATH="/etc/sudoers.d/remotica"
UDEV_PATH="/etc/udev/rules.d/99-remotica-serial.rules"
PORT=8080

INTERACTIVE=1
PURGE=0
MODE=install

case "$(basename "$0")" in
remotica-uninstall) MODE=uninstall ;;
esac

for arg in "$@"; do
    case "$arg" in
    --uninstall) MODE=uninstall ;;
    --purge) PURGE=1 ;;
    --no-interactive) INTERACTIVE=0 ;;
    -h | --help)
        echo "Usage: install.sh [--uninstall] [--purge] [--no-interactive]"
        exit 0
        ;;
    *)
        echo "Unknown option: $arg" >&2
        exit 1
        ;;
    esac
done

# Nothing to prompt with if there's no terminal — piping this script into
# `sh` is the documented way to run it, and a `read` there would block
# forever waiting for an answer nobody can give.
[ -t 0 ] || INTERACTIVE=0

say() { printf '%s\n' "$*"; }
step() { printf '  %-32s %s\n' "$1" "$2"; }
die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

require_root() {
    [ "$(id -u)" = 0 ] || die "must run as root — try: sudo $0 $*"
}

# ---------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------

do_uninstall() {
    require_root
    say ""
    say "=== Removing Remotica ==="

    if [ -f "$UNIT_PATH" ]; then
        systemctl stop "$UNIT_NAME" 2>/dev/null || true
        systemctl disable "$UNIT_NAME" 2>/dev/null || true
        rm -f "$UNIT_PATH"
        systemctl daemon-reload 2>/dev/null || true
        step "service" "stopped and removed"
    fi

    [ -f "$SUDOERS_PATH" ] && rm -f "$SUDOERS_PATH" && step "sudoers rule" "removed"

    if [ -f "$UDEV_PATH" ]; then
        rm -f "$UDEV_PATH"
        udevadm control --reload-rules 2>/dev/null || true
        step "udev rule" "removed"
    fi

    [ -d "$SHARE_DIR" ] && rm -rf "$SHARE_DIR" && step "dashboard files" "removed"

    if id -u "$SERVICE_USER" >/dev/null 2>&1; then
        userdel "$SERVICE_USER" 2>/dev/null || true
        step "service user" "removed"
    fi

    [ -f "$BIN_PATH" ] && rm -f "$BIN_PATH" && step "binary" "removed"

    # The data directory is the printer profile plus every gcode file
    # ever uploaded. Deleting that as a side effect of uninstalling would
    # be a nasty surprise, so it takes an explicit --purge.
    if [ -d "$DATA_DIR" ]; then
        if [ "$PURGE" = 1 ]; then
            if [ "$INTERACTIVE" = 1 ]; then
                printf 'Delete %s, including your printer profile and all uploaded gcode? [y/N] ' "$DATA_DIR"
                read -r reply
                case "$reply" in
                [Yy]*) rm -rf "$DATA_DIR" && step "data" "deleted" ;;
                *) step "data" "kept at $DATA_DIR" ;;
                esac
            else
                rm -rf "$DATA_DIR"
                step "data" "deleted"
            fi
        else
            step "data" "kept at $DATA_DIR"
        fi
    fi

    say ""
    if [ -d "$DATA_DIR" ]; then
        say "Remotica is uninstalled. Your printer profile and uploaded gcode"
        say "are still at $DATA_DIR — remove them with:"
        say ""
        say "    sudo rm -rf $DATA_DIR"
        say ""
    else
        say "Remotica is fully uninstalled."
    fi

    rm -f "$UNINSTALL_PATH"
    exit 0
}

[ "$MODE" = uninstall ] && do_uninstall

# ---------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------

require_root

case "$(uname -m)" in
x86_64) ARCH=x86_64 ;;
aarch64 | arm64) ARCH=aarch64 ;;
*) die "unsupported architecture '$(uname -m)' — Remotica ships builds for x86_64 and aarch64 only" ;;
esac

say ""
say "=== Installing Remotica ($ARCH) ==="

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT INT TERM

TARBALL="remotica-linux-$ARCH.tar.gz"
BASE_URL="https://github.com/$REPO/releases/latest/download"

# The documented way to run this is `curl ... | sudo sh`, where $0 is
# "sh" rather than a path — so this script may have no file on disk at
# all. Everything below that would otherwise use $0 has to cope with
# that; SELF is empty when it can't be located.
SELF=""
case "$0" in
*/*) [ -f "$0" ] && SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0") ;;
*) [ -f "./$0" ] && SELF=$(pwd)/$0 ;;
esac

# If the release payload is sitting next to this script, use it — that's
# how a downloaded-and-extracted release, or a locally built one,
# installs without touching the network. Only considered when this
# script actually exists on disk, so a piped run can't accidentally pick
# up whatever happens to be in the current directory.
LOCAL_DIR=""
[ -n "$SELF" ] && LOCAL_DIR=$(dirname "$SELF")
if [ -n "$LOCAL_DIR" ] && [ -f "$LOCAL_DIR/remotica" ] && [ -d "$LOCAL_DIR/web" ]; then
    SRC="$LOCAL_DIR"
    step "source" "local files in $LOCAL_DIR"
else
    command -v curl >/dev/null 2>&1 || die "curl is required to download the release"

    curl -fsSL -o "$WORK_DIR/$TARBALL" "$BASE_URL/$TARBALL" ||
        die "couldn't download $TARBALL from the latest release"
    curl -fsSL -o "$WORK_DIR/SHA256SUMS" "$BASE_URL/SHA256SUMS" ||
        die "couldn't download SHA256SUMS — refusing to install an unverified binary"

    # Verified BEFORE extracting: an unverified binary is never written
    # anywhere, let alone executed.
    (cd "$WORK_DIR" && grep " $TARBALL\$" SHA256SUMS | sha256sum -c -) >/dev/null 2>&1 ||
        die "checksum mismatch on $TARBALL — refusing to install"
    step "checksum" "verified"

    tar -xzf "$WORK_DIR/$TARBALL" -C "$WORK_DIR"
    SRC="$WORK_DIR/remotica-linux-$ARCH"
    [ -d "$SRC" ] || SRC="$WORK_DIR"
fi

[ -f "$SRC/remotica" ] || die "release archive is missing the remotica binary"
[ -d "$SRC/web" ] || die "release archive is missing the web/ directory"

if id -u "$SERVICE_USER" >/dev/null 2>&1; then
    step "service user" "already exists"
else
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    step "service user" "created '$SERVICE_USER'"
fi

install -m 0755 "$SRC/remotica" "$BIN_PATH"
step "binary" "$BIN_PATH"

# The uninstaller is this same script under another name. Prefer the copy
# from the release payload, then this script if it exists on disk — which
# it doesn't during a `curl | sh` run, hence the fallback order.
if [ -f "$SRC/install.sh" ]; then
    install -m 0755 "$SRC/install.sh" "$UNINSTALL_PATH"
    step "uninstaller" "$UNINSTALL_PATH"
elif [ -n "$SELF" ]; then
    install -m 0755 "$SELF" "$UNINSTALL_PATH"
    step "uninstaller" "$UNINSTALL_PATH"
else
    say "  WARNING: could not install remotica-uninstall (this script isn't on disk"
    say "           and the release payload didn't contain it)."
fi

# Removed first rather than copied over: a stale asset left behind from a
# previous version would still be served, and index.html references
# assets by content hash, so the mismatch would be silent.
rm -rf "$SHARE_DIR/web"
mkdir -p "$SHARE_DIR"
cp -r "$SRC/web" "$SHARE_DIR/web"
step "dashboard" "$SHARE_DIR/web"

# Created if absent, never emptied — this survives upgrades, and holds
# the printer profile and every uploaded gcode file.
if [ -d "$DATA_DIR" ]; then
    chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
    step "data" "kept existing $DATA_DIR"
else
    install -d -o "$SERVICE_USER" -g "$SERVICE_USER" -m 0755 "$DATA_DIR"
    step "data" "created $DATA_DIR"
fi

if [ -f "$SRC/99-remotica-serial.rules" ]; then
    install -m 0644 "$SRC/99-remotica-serial.rules" "$UDEV_PATH"
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger 2>/dev/null || true
    step "usb permissions" "udev rule installed"
fi

# A malformed file in /etc/sudoers.d/ can lock the entire machine out of
# sudo, so this is validated in a temp location and only moved into place
# once visudo says it parses.
if [ -f "$SRC/sudoers.remotica" ]; then
    cp "$SRC/sudoers.remotica" "$WORK_DIR/sudoers.check"
    chmod 0440 "$WORK_DIR/sudoers.check"
    if visudo -c -f "$WORK_DIR/sudoers.check" >/dev/null 2>&1; then
        install -m 0440 "$WORK_DIR/sudoers.check" "$SUDOERS_PATH"
        step "boot-start permission" "sudoers rule installed"
    else
        rm -f "$WORK_DIR/sudoers.check"
        say "  WARNING: the sudoers rule failed validation and was NOT installed."
        say "           Everything else works; the System page's start-on-boot"
        say "           toggle will report an error if used."
    fi
fi

install -m 0644 "$SRC/remotica.service" "$UNIT_PATH"
systemctl daemon-reload
step "service" "$UNIT_PATH"

# An upgrade must not silently change a decision already made, so this
# only asks when there's nothing to preserve.
if systemctl is-enabled "$UNIT_NAME" >/dev/null 2>&1; then
    step "start on boot" "already enabled, left as is"
elif [ "$INTERACTIVE" = 1 ]; then
    say ""
    printf 'Start Remotica automatically when this PC boots? [Y/n] '
    read -r reply
    case "${reply:-Y}" in
    [Yy]*)
        systemctl enable "$UNIT_NAME" >/dev/null 2>&1
        step "start on boot" "enabled"
        ;;
    *) step "start on boot" "left off — turn it on any time from the System page" ;;
    esac
else
    step "start on boot" "off (--no-interactive) — enable it from the System page"
fi

systemctl restart "$UNIT_NAME"

say ""
printf 'Waiting for Remotica to come up'
READY=0
i=0
while [ "$i" -lt 20 ]; do
    if curl -fsS -o /dev/null "http://localhost:$PORT/api/state" 2>/dev/null; then
        READY=1
        break
    fi
    printf '.'
    sleep 1
    i=$((i + 1))
done
printf '\n'

if [ "$READY" != 1 ]; then
    say ""
    say "Remotica did not become reachable. The last 50 log lines:"
    say ""
    journalctl -u "$UNIT_NAME" -n 50 --no-pager || true
    exit 1
fi

LAN_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
say ""
say "Remotica is running."
say ""
if [ -n "$LAN_IP" ]; then
    say "    Open it from any device on your network:  http://$LAN_IP:$PORT"
else
    say "    Open it on this machine:  http://localhost:$PORT"
fi
say ""
say "  Anyone on your network who opens that URL can control your printer."
say "  Remotica has no login of any kind. Only run it on a network you trust."
say ""
say "  Status:     systemctl status $UNIT_NAME"
say "  Logs:       journalctl -u $UNIT_NAME -f"
say "  Uninstall:  sudo remotica-uninstall"
say ""
