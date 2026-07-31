#!/usr/bin/env python3
"""
Fake-firmware test harness for transport_serial.c's checksum protocol.

There's no real printer available, so this creates a Linux pseudo-terminal
pair, points the real backend at the slave end via --serial, and acts as a
(minimal, fake) Marlin firmware on the master end. This exercises the ACTUAL
serial code path (open/termios/write_line/read_line/checksums) instead of
the simulator, which has no real I/O at all.

Two scenarios:
  A. Firmware supports M110/checksums -> negotiation succeeds, print lines
     arrive wrapped as "N<n> <cmd>*<checksum>", checksums are verified
     against an independently-computed reference, and one line is
     deliberately Resend-requested once to prove the retry path works.
  B. Firmware doesn't answer M110 -> negotiation fails -> fallback to plain
     unnumbered lines, verified by checking sent lines have no "N" prefix.

Usage: build the backend first (`cd backend && make`), make sure nothing
else is listening on port 8080, then from the repo root:
    python3 backend/tools/fake_marlin_test.py
"""
import os
import pty
import re
import subprocess
import sys
import time
import tty
import urllib.request
import urllib.error

BACKEND_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BACKEND_BIN = os.path.join(BACKEND_DIR, "build/remotica-backend")
API_BASE = "http://localhost:8080"


def gcode_checksum(s: str) -> int:
    sum_ = 0
    for ch in s:
        sum_ ^= ord(ch)
    return sum_


class FakeFirmware:
    """Owns the pty master fd; reads lines the backend sends, replies."""

    def __init__(self, master_fd, respond_to_m110=True, resend_once_on_line_containing=None):
        self.master_fd = master_fd
        self.respond_to_m110 = respond_to_m110
        self.resend_once_on_line_containing = resend_once_on_line_containing
        self._resent_already = False
        self.buf = b""
        self.received_raw_lines = []

    def _read_line(self, timeout=6.0):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            r, _, _ = __import__("select").select([self.master_fd], [], [], remaining)
            if not r:
                return None
            try:
                chunk = os.read(self.master_fd, 4096)
            except OSError:
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode(errors="replace").rstrip("\r")

    def _write(self, text):
        os.write(self.master_fd, (text + "\n").encode())

    def send_boot_banner(self):
        # Simulated boot-time noise, like a real Arduino-compatible board
        # resetting on DTR toggle would emit. Tests that this gets
        # correctly discarded (see the P1-7 tcflush-after-sleep fix)
        # instead of being misread as a reply to the first real command.
        os.write(self.master_fd, b"start\r\necho:FakeMarlin bootup noise\r\n")

    def handle_one(self):
        """Reads and responds to exactly one command line. Returns the
        raw line received, or None on timeout/EOF."""
        line = self._read_line()
        if line is None:
            return None
        self.received_raw_lines.append(line)

        if line.startswith("M115"):
            self._write("FIRMWARE_NAME:FakeMarlin 1.0 FOR_TESTING MACHINE_TYPE:PtyFake")
            self._write("ok")
            return line

        if line.startswith("M110"):
            if self.respond_to_m110:
                self._write("ok")
            else:
                # Deliberately don't answer at all -> the backend's
                # 5s wait-for-ok times out -> checksums_enabled stays 0.
                pass
            return line

        # A numbered/checksummed line looks like: "N5 G1 X10 Y10*37"
        m = re.match(r"^N(\d+) (.*)\*(\d+)$", line)
        if m:
            n, body, checksum = m.group(1), m.group(2), int(m.group(3))
            expected = gcode_checksum(f"N{n} {body}")
            if expected != checksum:
                self._write(f"Error:checksum mismatch, Last Line: {n}")
                self._write(f"Resend: {n}")
                return line

            if (
                self.resend_once_on_line_containing
                and self.resend_once_on_line_containing in body
                and not self._resent_already
            ):
                self._resent_already = True
                self._write(f"Resend: {n}")
                return line

            self._write("ok")
            return line

        # Any other bare/unnumbered command (jog, temp, print lines when
        # checksums aren't enabled, etc.) -> just accept it, like real
        # Marlin does for unnumbered lines regardless of checksum state.
        self._write("ok")
        return line


def http_post(path, data=None):
    req = urllib.request.Request(API_BASE + path, data=data or b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def http_get(path):
    with urllib.request.urlopen(API_BASE + path, timeout=5) as resp:
        return resp.status, resp.read()


def wait_for_backend_ready(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status, _ = http_get("/api/state")
            if status == 200:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def run_scenario(label, respond_to_m110, resend_once_on_line_containing):
    print(f"\n=== Scenario: {label} ===")
    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    print(f"pty slave: {slave_path}")

    # A fresh pty defaults to canonical/echo mode (like an interactive
    # terminal) — without this, writing the boot banner to master_fd
    # would just get echoed straight back to our own read on master_fd,
    # before the backend even opens its end. Raw mode disables that.
    tty.setraw(slave_fd)

    fw = FakeFirmware(master_fd, respond_to_m110, resend_once_on_line_containing)
    fw.send_boot_banner()

    proc = subprocess.Popen(
        [BACKEND_BIN, "--serial", slave_path],
        cwd=BACKEND_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        # Handle M115 then M110 as the backend's connect sequence sends them.
        got_m115 = fw.handle_one()
        got_m110 = fw.handle_one()
        print(f"  backend sent (connect): {got_m115!r}, {got_m110!r}")

        if not wait_for_backend_ready():
            print("  FAIL: backend never became reachable over HTTP")
            return False

        status, body = http_get("/api/state")
        print(f"  GET /api/state -> {status} {body[:200]}")
        if b'"connected":true' not in body:
            print("  FAIL: driver did not report connected")
            return False

        gcode = "G90\nM82\nG1 X10 Y10\nG1 X20 Y20 RESENDME\nG1 X30 Y30\n"
        status, body = http_post(
            "/api/upload?filename=pty_test.gcode", data=gcode.encode()
        )
        print(f"  upload -> {status} {body[:200]}")
        if status != 200:
            print("  FAIL: upload rejected")
            return False

        status, body = http_post("/api/print/start")
        print(f"  print/start -> {status} {body[:200]}")
        if status != 200:
            print("  FAIL: print/start rejected")
            return False

        # Drive the fake-firmware side through the print: G90, M82, then
        # 3 G1 lines (one of which may need a resend), matching the file.
        expected_commands = 5
        handled = 0
        resend_seen = False
        while handled < expected_commands + (1 if resend_once_on_line_containing else 0):
            line = fw.handle_one()
            if line is None:
                print(f"  FAIL: firmware side got no more lines after {handled} (timeout)")
                return False
            if line.startswith("Resend") or "Resend" in line:
                pass
            handled += 1
            print(f"  firmware received: {line!r}")

        # Confirm the print actually finished from the backend's point of view.
        deadline = time.time() + 5
        final_status = None
        while time.time() < deadline:
            _, body = http_get("/api/state")
            if b'"status":"ready"' in body:
                final_status = "ready"
                break
            time.sleep(0.2)
        print(f"  final job status: {final_status}")
        if final_status != "ready":
            print("  FAIL: print never reached 'ready' (completed) status")
            return False

        sent_lines = [l for l in fw.received_raw_lines if l not in ("M115", "M110 N0")]
        print(f"  all lines firmware saw (excluding M115/M110): {sent_lines}")

        if respond_to_m110:
            if not all(l.startswith("N") for l in sent_lines):
                print("  FAIL: expected all print lines to be checksummed/numbered")
                return False
            print("  OK: all print lines were numbered+checksummed")
            if fw.resend_once_on_line_containing and not fw._resent_already:
                print("  FAIL: resend scenario never triggered")
                return False
            if fw.resend_once_on_line_containing:
                print("  OK: Resend was requested and the retried line was accepted")
        else:
            if any(l.startswith("N") for l in sent_lines):
                print("  FAIL: expected fallback to PLAIN lines (no checksums), but saw N-prefixed lines")
                return False
            print("  OK: fell back to plain unnumbered lines, exactly as before this feature")

        print(f"  PASS: {label}")
        return True

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        out = proc.stdout.read() if proc.stdout else ""
        if out.strip():
            print("  --- backend stdout/stderr (last 20 lines) ---")
            for l in out.strip().splitlines()[-20:]:
                print("   ", l)
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    results = []
    results.append(
        run_scenario(
            "A: negotiation succeeds, checksums verified, one deliberate Resend",
            respond_to_m110=True,
            resend_once_on_line_containing="X20",
        )
    )
    results.append(
        run_scenario(
            "B: firmware doesn't support M110 -> fallback to plain lines",
            respond_to_m110=False,
            resend_once_on_line_containing=None,
        )
    )

    print("\n=== SUMMARY ===")
    print(f"Scenario A: {'PASS' if results[0] else 'FAIL'}")
    print(f"Scenario B: {'PASS' if results[1] else 'FAIL'}")
    sys.exit(0 if all(results) else 1)
