#!/usr/bin/env python3
"""
Fake-firmware test harness for transport_serial.c's line-level protocol.

There's no real printer available, so this creates a Linux pseudo-terminal
pair, points the real backend at the slave end via --serial, and acts as a
(minimal, fake) Marlin firmware on the master end. This exercises the ACTUAL
serial code path (open/termios/write_line/read_line/checksums) instead of
the simulator, which has no real I/O at all.

The fake firmware implements Marlin's line-numbering rules properly, which
matters more than it sounds: a line number it has already seen is re-acked
WITHOUT the new content being executed, exactly like real Marlin. Scenario D
below depends on that, and so does anything else that could regress into
"the backend thinks it sent a command that the printer quietly dropped".

Four scenarios:
  A. Firmware supports M110/checksums -> negotiation succeeds, print lines
     arrive wrapped as "N<n> <cmd>*<checksum>", checksums are verified
     against an independently-computed reference, and one line is
     deliberately Resend-requested once to prove the retry path works.
  B. Firmware doesn't answer M110 -> negotiation fails -> fallback to plain
     unnumbered lines, verified by checking sent lines have no "N" prefix.
  C. A legitimately slow move: the firmware answers with nothing but
     "echo:busy: processing" keepalives for well over the 5s normal command
     timeout before finally sending "ok". The print must survive this.
     Regression test for the false-abort bug found against a real Ender 3
     (2026-08-01) — a travel move after G28 took just over 5s and killed
     the whole print.
  D. A line the firmware accepts but never acknowledges -> the backend
     times out, aborts the print, and runs its abort-safety sequence
     (heaters off, fan off, Z lift, steppers off). Those commands must
     ACTUALLY REACH the firmware. Regression test for the safety bug found
     on the same day: the safety commands were being sent under the failed
     line's already-used line number, so Marlin re-acked them without
     executing any of them and the heaters stayed at full temperature.

Usage: build the backend first (`cd backend && make`), then from anywhere:
    python3 backend/tools/fake_marlin_test.py
It binds its own HTTP port (see API_PORT) rather than the default 8080, so
it can run while a real Remotica backend is already serving the dashboard.
"""
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty
import urllib.request
import urllib.error

BACKEND_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BACKEND_BIN = os.path.join(BACKEND_DIR, "build/remotica-backend")

# Deliberately not 8080: a real backend driving a real printer is often
# already running on the default port, and this harness must never require
# stopping it. Passed through to the backend via --port.
API_PORT = 8099
API_BASE = f"http://localhost:{API_PORT}"

# Must stay in step with TIMEOUT_MS_NORMAL in transport_serial.c — the
# scenarios below are built around being deliberately slower/faster than it.
NORMAL_TIMEOUT_S = 5.0


def gcode_checksum(s: str) -> int:
    sum_ = 0
    for ch in s:
        sum_ ^= ord(ch)
    return sum_


class FakeFirmware:
    """Owns the pty master fd; reads lines the backend sends, replies.

    Runs on its own thread (see start()/stop()) rather than being pumped
    one line at a time by the test body. That's necessary now that some
    scenarios take tens of seconds: the backend's 300ms tick thread slips
    M105 temperature polls in between print lines, and a test that expected
    an exact sequence of lines would break on them.
    """

    def __init__(
        self,
        master_fd,
        respond_to_m110=True,
        resend_once_on=None,
        busy_on=None,
        busy_seconds=0.0,
        silent_on=None,
    ):
        self.master_fd = master_fd
        self.respond_to_m110 = respond_to_m110
        self.resend_once_on = resend_once_on
        self.busy_on = busy_on
        self.busy_seconds = busy_seconds
        self.silent_on = silent_on

        self._resent_already = False
        self.buf = b""
        self.lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

        # Everything below is read by the test body while the pump thread
        # writes it, so it's all guarded by self.lock.
        self.received_raw_lines = []  # every line the backend sent, verbatim
        self.executed = []  # command bodies actually acted on
        self.ignored_duplicates = []  # (n, body) re-acked but NOT executed
        self.checksum_errors = []
        self.m110_count = 0
        self.last_n = 0  # Marlin's gcode_LastN

    # --- wire I/O ---------------------------------------------------

    def _read_line(self, timeout):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            r, _, _ = select.select([self.master_fd], [], [], remaining)
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
        try:
            os.write(self.master_fd, (text + "\n").encode())
        except OSError:
            pass  # backend went away mid-scenario; the test body will notice

    def send_boot_banner(self):
        # Simulated boot-time noise, like a real Arduino-compatible board
        # resetting on DTR toggle would emit. Tests that this gets
        # correctly discarded (see the P1-7 tcflush-after-sleep fix)
        # instead of being misread as a reply to the first real command.
        os.write(self.master_fd, b"start\r\necho:FakeMarlin bootup noise\r\n")

    # --- thread lifecycle -------------------------------------------

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def _run(self):
        while not self._stop.is_set():
            self.handle_one(timeout=0.3)

    # --- snapshot helpers used by assertions ------------------------

    def snapshot(self):
        with self.lock:
            return {
                "raw": list(self.received_raw_lines),
                "executed": list(self.executed),
                "ignored_duplicates": list(self.ignored_duplicates),
                "checksum_errors": list(self.checksum_errors),
                "m110_count": self.m110_count,
            }

    def executed_contains(self, command):
        with self.lock:
            return command in self.executed

    # --- the fake firmware itself -----------------------------------

    def handle_one(self, timeout=1.0):
        """Reads and responds to at most one command line. Returns the raw
        line received, or None on timeout/EOF."""
        line = self._read_line(timeout)
        if line is None:
            return None
        with self.lock:
            self.received_raw_lines.append(line)

        if line.startswith("M115"):
            self._write("FIRMWARE_NAME:FakeMarlin 1.0 FOR_TESTING MACHINE_TYPE:PtyFake")
            self._write("ok")
            return line

        if line.startswith("M110"):
            if self.respond_to_m110:
                # M110 N0 means "the next line will be N1". This is the one
                # thing that legitimately rewinds the sequence — everything
                # else must be strictly increasing.
                with self.lock:
                    self.last_n = 0
                    self.m110_count += 1
                self._write("ok")
            else:
                # Deliberately don't answer at all -> the backend's
                # 5s wait-for-ok times out -> checksums_enabled stays 0.
                pass
            return line

        # A numbered/checksummed line looks like: "N5 G1 X10 Y10*37"
        m = re.match(r"^N(\d+) (.*)\*(\d+)$", line)
        if m:
            n, body, checksum = int(m.group(1)), m.group(2), int(m.group(3))
            expected = gcode_checksum(f"N{n} {body}")
            if expected != checksum:
                with self.lock:
                    self.checksum_errors.append(line)
                self._write(f"Error:checksum mismatch, Last Line: {self.last_n}")
                self._write(f"Resend: {self.last_n + 1}")
                return line

            with self.lock:
                last_n = self.last_n

            if n <= last_n:
                # THE important rule. Real Marlin treats an already-seen
                # line number as a retransmission of something it already
                # has: it acknowledges it and moves on WITHOUT parsing the
                # body. Whatever new command was in there is silently lost.
                # Reproducing this faithfully is what makes scenario D able
                # to catch the stale-line-number safety bug at all.
                with self.lock:
                    self.ignored_duplicates.append((n, body))
                self._write("ok")
                return line

            if n > last_n + 1:
                self._write(f"Resend: {last_n + 1}")
                return line

            if self.resend_once_on and self.resend_once_on in body and not self._resent_already:
                # Ask for it again without accepting it — last_n stays put,
                # so the resent copy arrives as the same, in-sequence N.
                self._resent_already = True
                self._write(f"Resend: {n}")
                return line

            with self.lock:
                self.last_n = n
            self._respond_to_body(body)
            return line

        # Any other bare/unnumbered command (jog, temp, M105 polls, print
        # lines when checksums aren't enabled, etc.) -> handled normally,
        # like real Marlin does for unnumbered lines regardless of checksum
        # state.
        self._respond_to_body(line)
        return line

    def _respond_to_body(self, body):
        if self.silent_on and self.silent_on in body:
            # Accepted (last_n has already advanced) but deliberately never
            # acknowledged — the printer heard us and then went quiet, which
            # is what a stalled/wedged board looks like from the host side.
            # The backend must time out and abort the print.
            with self.lock:
                self.executed.append(body)
            return

        if self.busy_on and self.busy_on in body:
            # A long but perfectly legitimate move: nothing but keepalives
            # for busy_seconds, then a normal ok. Real Marlin emits these
            # roughly every 2s while a command is still executing.
            end = time.time() + self.busy_seconds
            while time.time() < end and not self._stop.is_set():
                time.sleep(1.0)
                self._write("echo:busy: processing")

        with self.lock:
            self.executed.append(body)

        if body.startswith("M105"):
            self._write("T:200.00 /200.00 B:60.00 /60.00 @:0 B@:0")

        self._write("ok")


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


def wait_for_backend_ready(timeout=20):
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


def wait_for_connected(timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            _, body = http_get("/api/state")
            if b'"connected":true' in body:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def wait_for_job_status(status_name, timeout):
    """Waits for the job to settle into `status_name`. Returns True/False."""
    needle = f'"status":"{status_name}"'.encode()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            _, body = http_get("/api/state")
            if needle in body:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def upload_and_start(gcode, filename):
    status, body = http_post(f"/api/upload?filename={filename}", data=gcode.encode())
    if status != 200:
        print(f"  FAIL: upload rejected ({status} {body[:200]})")
        return False
    status, body = http_post("/api/print/start")
    if status != 200:
        print(f"  FAIL: print/start rejected ({status} {body[:200]})")
        return False
    return True


class Session:
    """A pty pair + fake firmware + a backend process pointed at it."""

    def __init__(self, **firmware_kwargs):
        self.firmware_kwargs = firmware_kwargs

    def __enter__(self):
        self.master_fd, self.slave_fd = pty.openpty()
        self.slave_path = os.ttyname(self.slave_fd)
        print(f"  pty slave: {self.slave_path}")

        # A fresh pty defaults to canonical/echo mode (like an interactive
        # terminal) — without this, writing the boot banner to master_fd
        # would just get echoed straight back to our own read on master_fd,
        # before the backend even opens its end. Raw mode disables that.
        tty.setraw(self.slave_fd)

        self.fw = FakeFirmware(self.master_fd, **self.firmware_kwargs)
        self.fw.send_boot_banner()
        self.fw.start()

        # Run in a throwaway working directory, not backend/. The backend
        # resolves data/profile.json and data/uploads/ relative to its cwd
        # and creates them on demand, so this keeps the test's uploaded
        # gcode out of a real backend's file list.
        self.workdir = tempfile.mkdtemp(prefix="remotica-pty-test-")

        self.proc = subprocess.Popen(
            [BACKEND_BIN, "--serial", self.slave_path, "--port", str(API_PORT)],
            cwd=self.workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        return self

    def __exit__(self, *exc):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.fw.stop()
        out = self.proc.stdout.read() if self.proc.stdout else ""
        if out.strip():
            print("  --- backend stdout/stderr (last 20 lines) ---")
            for l in out.strip().splitlines()[-20:]:
                print("   ", l)
        os.close(self.master_fd)
        os.close(self.slave_fd)
        shutil.rmtree(self.workdir, ignore_errors=True)
        return False


def print_lines_only(raw):
    """The raw lines that were actual print/command traffic — i.e. not the
    connect handshake and not the tick thread's temperature polls."""
    return [
        l
        for l in raw
        if not l.startswith("M115") and not l.startswith("M110") and "M105" not in l
    ]


# ---------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------


def scenario_checksums(label, respond_to_m110, resend_once_on):
    """Scenarios A and B: negotiation succeeds (numbered lines, one Resend)
    or fails (fallback to plain lines)."""
    print(f"\n=== Scenario: {label} ===")
    with Session(respond_to_m110=respond_to_m110, resend_once_on=resend_once_on) as s:
        if not wait_for_backend_ready():
            print("  FAIL: backend never became reachable over HTTP")
            return False
        if not wait_for_connected():
            print("  FAIL: driver did not report connected")
            return False

        gcode = "G90\nM82\nG1 X10 Y10\nG1 X20 Y20 RESENDME\nG1 X30 Y30\n"
        if not upload_and_start(gcode, "pty_test.gcode"):
            return False

        if not wait_for_job_status("ready", timeout=20):
            print("  FAIL: print never reached 'ready' (completed) status")
            return False

        snap = s.fw.snapshot()
        sent = print_lines_only(snap["raw"])
        print(f"  print lines the firmware saw: {sent}")

        if snap["checksum_errors"]:
            print(f"  FAIL: firmware rejected checksums: {snap['checksum_errors']}")
            return False

        if respond_to_m110:
            if not all(l.startswith("N") for l in sent):
                print("  FAIL: expected all print lines to be checksummed/numbered")
                return False
            print("  OK: all print lines were numbered+checksummed")
            if resend_once_on and not s.fw._resent_already:
                print("  FAIL: resend scenario never triggered")
                return False
            if resend_once_on:
                print("  OK: Resend was requested and the retried line was accepted")
            if snap["ignored_duplicates"]:
                print(
                    "  FAIL: firmware silently dropped duplicate-numbered lines: "
                    f"{snap['ignored_duplicates']}"
                )
                return False
        else:
            if any(l.startswith("N") for l in sent):
                print("  FAIL: expected fallback to PLAIN lines (no checksums), but saw N-prefixed")
                return False
            print("  OK: fell back to plain unnumbered lines, exactly as before this feature")

        print(f"  PASS: {label}")
        return True


def scenario_busy_keepalive():
    """Scenario C: a move that legitimately takes longer than the normal
    5s command timeout, with the firmware busy-keepaliving throughout.

    Before the fix, the deadline was computed once and never extended, so
    the busy lines were skipped over as noise and the command was declared
    a driver failure at 5s — aborting the entire print after one move.
    """
    label = "C: slow move + busy keepalives must not abort the print"
    print(f"\n=== Scenario: {label} ===")
    busy_seconds = NORMAL_TIMEOUT_S + 4.0  # comfortably past the timeout

    with Session(respond_to_m110=True, busy_on="SLOWMOVE", busy_seconds=busy_seconds) as s:
        if not wait_for_backend_ready():
            print("  FAIL: backend never became reachable over HTTP")
            return False
        if not wait_for_connected():
            print("  FAIL: driver did not report connected")
            return False

        gcode = "G90\nG1 X10 Y10\nG1 X99 Y99 SLOWMOVE\nG1 X30 Y30\n"
        if not upload_and_start(gcode, "pty_busy.gcode"):
            return False

        started = time.time()
        # Generous: busy_seconds of stalling plus normal streaming.
        if not wait_for_job_status("ready", timeout=busy_seconds + 25):
            print("  FAIL: print did not complete — the slow move was treated as a failure")
            return False
        elapsed = time.time() - started
        print(f"  print completed in {elapsed:.1f}s (slow move alone stalled {busy_seconds:.0f}s)")

        if elapsed < NORMAL_TIMEOUT_S:
            print("  FAIL: finished too fast — the slow move can't actually have been waited out")
            return False

        snap = s.fw.snapshot()
        for expected in ("G1 X99 Y99 SLOWMOVE", "G1 X30 Y30"):
            if expected not in snap["executed"]:
                print(f"  firmware executed: {snap['executed']}")
                print(f"  FAIL: firmware never executed {expected!r}")
                print("        (the print was aborted mid-file — note the job still reports")
                print("         'ready' after a driver-failure abort, so only the executed")
                print("         command list can tell a completed print from an aborted one)")
                return False
        print("  OK: the slow line AND the line after it were both executed")
        print(f"  PASS: {label}")
        return True


def scenario_abort_safety():
    """Scenario D: a line the firmware accepts but never acks, forcing a
    driver-failure abort — then the abort-safety sequence must actually
    reach the printer.

    Before the fix, the failed line's number was never retired, so the
    safety commands went out under an already-seen N and Marlin re-acked
    them without executing any of them. Live consequence: hotend and bed
    stayed at full print temperature after an "aborted" print, with the
    backend reporting the shutdown as successful.
    """
    label = "D: abort-safety heater-off must actually reach the firmware"
    print(f"\n=== Scenario: {label} ===")

    with Session(respond_to_m110=True, silent_on="STALLME") as s:
        if not wait_for_backend_ready():
            print("  FAIL: backend never became reachable over HTTP")
            return False
        if not wait_for_connected():
            print("  FAIL: driver did not report connected")
            return False

        gcode = "G90\nM82\nG1 X10 Y10\nG1 X20 Y20 STALLME\nG1 X30 Y30\n"
        if not upload_and_start(gcode, "pty_stall.gcode"):
            return False

        # The stalled line burns the normal timeout, then the abort-safety
        # sequence runs (7 commands, each fast once numbering is resynced).
        if not wait_for_job_status("ready", timeout=NORMAL_TIMEOUT_S + 30):
            print("  FAIL: job never settled after the stalled line")
            return False

        # Give the safety sequence a moment to finish arriving.
        deadline = time.time() + 15
        while time.time() < deadline and not s.fw.executed_contains("M84"):
            time.sleep(0.2)

        snap = s.fw.snapshot()
        print(f"  firmware executed: {snap['executed']}")
        print(f"  firmware silently dropped (duplicate N): {snap['ignored_duplicates']}")
        print(f"  M110 negotiations seen: {snap['m110_count']}")

        required = ["M104 S0", "M140 S0", "M107"]
        missing = [c for c in required if c not in snap["executed"]]
        if missing:
            print(f"  FAIL: abort-safety commands never executed by the firmware: {missing}")
            print("        (this is the stale-line-number bug: they were acked but dropped)")
            return False
        print("  OK: heaters-off and fan-off all actually reached the firmware")

        if snap["ignored_duplicates"]:
            print(
                "  FAIL: some commands were re-acked under an already-used line number "
                f"and silently dropped: {snap['ignored_duplicates']}"
            )
            return False
        print("  OK: nothing was silently dropped as a duplicate line number")

        if snap["m110_count"] < 2:
            print(
                "  FAIL: expected a second M110 negotiation (the post-failure resync), "
                f"only saw {snap['m110_count']}"
            )
            return False
        print("  OK: line numbering was explicitly resynchronised (M110) before the safety lines")

        if "M84" not in snap["executed"]:
            print("  FAIL: the park/release half of the safety sequence never completed")
            return False
        print("  OK: Z lift + steppers-off also completed")

        print(f"  PASS: {label}")
        return True


if __name__ == "__main__":
    if not os.path.exists(BACKEND_BIN):
        print(f"Backend binary not found at {BACKEND_BIN} — run `cd backend && make` first.")
        sys.exit(1)

    # Optional argv filter, e.g. `fake_marlin_test.py C D` to run just the
    # two regression scenarios. No arguments runs everything.
    wanted = {a.upper() for a in sys.argv[1:]} or {"A", "B", "C", "D"}

    label_a = "A: negotiation succeeds, checksums verified, one deliberate Resend"
    label_b = "B: firmware doesn't support M110 -> fallback to plain lines"

    results = []
    if "A" in wanted:
        results.append(
            (label_a, scenario_checksums(label_a, respond_to_m110=True, resend_once_on="X20"))
        )
    if "B" in wanted:
        results.append(
            (label_b, scenario_checksums(label_b, respond_to_m110=False, resend_once_on=None))
        )
    if "C" in wanted:
        results.append(("C: slow move + busy keepalives", scenario_busy_keepalive()))
    if "D" in wanted:
        results.append(("D: abort-safety actually reaches the printer", scenario_abort_safety()))

    print("\n=== SUMMARY ===")
    for name, ok in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    sys.exit(0 if all(ok for _, ok in results) else 1)
