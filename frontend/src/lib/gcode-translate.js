// Translates gcode into a plain-English description: the fixed set this
// backend's jog/home/temp panel and sim tick send directly, plus the
// commands that show up when streaming a real, sliced gcode file (see
// job_manager.c's streamer thread) — those are forwarded to the driver
// (and so to this console) verbatim, so covering "whatever this backend
// deliberately sends" is no longer enough on its own.
//
// Not a general gcode parser: this is a lookup table for the commands
// that actually show up in practice (checked against a real Cura-sliced
// file's command vocabulary), not an attempt to understand arbitrary
// gcode. Anything outside that returns null, so callers fall back to
// showing the raw line instead of a wrong guess.

const AXIS_LABELS = { X: "X", Y: "Y", Z: "Z", E: "the extruder" };
const AXIS_ORDER = ["X", "Y", "Z", "E"];

function parseAxisParams(paramsText) {
  const params = {};
  let feedRate = null;
  for (const token of paramsText.trim().split(/\s+/)) {
    const letter = token[0];
    const value = Number(token.slice(1));
    if (letter === "F") feedRate = value;
    else if (letter in AXIS_LABELS) params[letter] = value;
  }
  return { params, feedRate };
}

function translateMove(command, paramsText) {
  const { params, feedRate } = parseAxisParams(paramsText);
  const axisEntries = Object.entries(params);
  if (axisEntries.length === 0) return null;

  // A single-axis G1 with no other axis present is exactly how this
  // backend's own jog buttons move the printer (see transport_sim.c's
  // sim_jog / transport_serial.c's serial_jog) — always one axis,
  // immediately wrapped in G91/G90 — so it's safe to describe as a
  // relative delta. Anything else (G0, or several axes on one line) is
  // real print-file gcode, which uses absolute coordinates in virtually
  // every slicer's default output — described as a target position
  // instead, since single-line translation has no way to track G90/G91
  // modal state across the whole file to know for certain.
  if (command === "G1" && axisEntries.length === 1) {
    const [axis, delta] = axisEntries[0];
    const sign = delta > 0 ? "+" : "";
    return `Move ${AXIS_LABELS[axis]} by ${sign}${delta.toFixed(1)}mm`;
  }

  const target = AXIS_ORDER.filter((axis) => axis in params)
    .map((axis) => `${axis} ${params[axis].toFixed(2)}mm`)
    .join(", ");
  const feedText =
    feedRate != null ? ` at feed ${feedRate.toFixed(0)}mm/min` : "";
  return `Move to ${target}${feedText}`;
}

function translateSetPosition(paramsText) {
  if (!paramsText) return "Reset all axis positions to 0";
  const { params } = parseAxisParams(paramsText);
  const entries = AXIS_ORDER.filter((axis) => axis in params).map(
    (axis) => `${axis} ${params[axis].toFixed(2)}mm`
  );
  return entries.length > 0
    ? `Set current position: ${entries.join(", ")}`
    : null;
}

function translateSent(text) {
  if (text === "G91") return "Switch to relative positioning";
  if (text === "G90") return "Switch to absolute positioning";
  if (text === "G28") return "Home all axes";
  if (text === "M105") return "Request a temperature report";
  if (text === "M107") return "Turn the part-cooling fan off";
  if (text === "M84") return "Disable stepper motors";
  if (text === "M82") return "Switch extruder to absolute positioning";
  if (text === "M83") return "Switch extruder to relative positioning";

  // Sent unnumbered at connect, and again after a failed line, to
  // resynchronise the line numbering — see resync_line_numbers() in
  // transport_serial.c. Cryptic enough to be worth naming, and it now
  // shows up in the console whenever a resend recovery happens.
  const setLineNumber = text.match(/^M110(?:\s+N(-?\d+))?$/);
  if (setLineNumber) {
    return `Reset command numbering to ${setLineNumber[1] ?? 0}`;
  }

  const move = text.match(/^(G0|G1)((?:\s+[XYZEF]-?[\d.]+)+)$/);
  if (move) return translateMove(move[1], move[2]);

  const setPosition = text.match(/^G92((?:\s+[XYZEF]-?[\d.]+)+)?$/);
  if (setPosition) return translateSetPosition(setPosition[1]);

  const fanOn = text.match(/^M106(?: S(-?[\d.]+))?$/);
  if (fanOn) {
    const speed = fanOn[1] != null ? Number(fanOn[1]) : 255;
    return `Set part-cooling fan speed to ${Math.round((speed / 255) * 100)}%`;
  }

  const hotendTemp = text.match(/^M104 S(-?[\d.]+)$/);
  if (hotendTemp) {
    return `Set hotend target temperature to ${Number(hotendTemp[1]).toFixed(0)}°C`;
  }

  const bedTemp = text.match(/^M140 S(-?[\d.]+)$/);
  if (bedTemp) {
    return `Set bed target temperature to ${Number(bedTemp[1]).toFixed(0)}°C`;
  }

  const hotendTempWait = text.match(/^M109 S(-?[\d.]+)$/);
  if (hotendTempWait) {
    return `Set hotend target temperature to ${Number(hotendTempWait[1]).toFixed(0)}°C and wait`;
  }

  const bedTempWait = text.match(/^M190 S(-?[\d.]+)$/);
  if (bedTempWait) {
    return `Set bed target temperature to ${Number(bedTempWait[1]).toFixed(0)}°C and wait`;
  }

  return null;
}

function translateReceived(text) {
  // Checked before the bare "ok" case below: Marlin's actual reply to
  // M105 looks like "ok T:205.32 /210.00 B:59.81 /60.00 @:87 B@:32" — it
  // starts with "ok" too, so if the plain acknowledgement check ran
  // first it would always win and this more specific, more useful
  // translation would never be reached.
  const tempReport = text.match(
    /T:(-?[\d.]+)\s*\/(-?[\d.]+).*?B:(-?[\d.]+)\s*\/(-?[\d.]+)/
  );
  if (tempReport) {
    const [, hotendCurrent, hotendTarget, bedCurrent, bedTarget] = tempReport;
    return `Reported temperatures — hotend ${hotendCurrent}°C (target ${hotendTarget}°C), bed ${bedCurrent}°C (target ${bedTarget}°C)`;
  }

  if (/^ok\b/i.test(text)) {
    return "Acknowledged — ready for the next command";
  }

  // The other half of the checksum protocol: the printer saw a corrupted
  // or out-of-sequence line and wants it again. Worth naming, because a
  // run of these is the visible symptom of a bad USB cable.
  const resend = text.match(/^Resend:\s*(\d+)/i);
  if (resend) {
    return `Line ${resend[1]} arrived corrupted — printer asked for it again`;
  }

  // Marlin's host-keepalive during a long move. Reads as a stall
  // otherwise; it's the printer explicitly saying it's still working.
  if (/^echo:\s*busy:\s*processing/i.test(text)) {
    return "Still working on the previous command";
  }

  return null;
}

// Strips the RepRap line-number prefix and checksum suffix that
// transport_serial.c wraps around every line of a streamed print — so
// "N4213 G1 X10.5 Y20.2 F1200*45" is matched below as "G1 X10.5 Y20.2
// F1200".
//
// This matters more than it looks. Every matcher in translateSent is
// anchored (exact equality, or /^...$/), so the N-prefix broke the start
// and the checksum broke the end: during a real print — the one time
// this console is genuinely useful — not a single streamed line
// translated, and Plain English appeared to do nothing at all. Jog, home
// and temperature commands were unaffected, because those go through the
// non-checksummed path (send_and_wait_for_ok), which is why this looks
// fine against the simulator.
//
// Deliberately stripped here rather than in the backend: the console's
// raw view is a protocol view, and the real wire bytes — line numbers
// and checksums included — are exactly what you need when diagnosing a
// resend storm or the stale-line-number class of bug this driver has
// already hit once. So the framing stays visible with the toggle off,
// and is only removed for the purpose of describing the command.
function stripLineFraming(text) {
  return text
    .replace(/^N\d+\s+/, "")
    .replace(/\*\d+$/, "")
    .trim();
}

export function translateGcodeLine(text, direction) {
  const trimmed = text.trim();
  return direction === "sent"
    ? translateSent(stripLineFraming(trimmed))
    : translateReceived(trimmed);
}
