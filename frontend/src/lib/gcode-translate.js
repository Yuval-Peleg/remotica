// Translates the small, fixed vocabulary of gcode this backend actually
// sends and receives (see backend/src/transport_sim.c and
// transport_serial.c — they only ever produce G90/G91/G1/G28/M104/M140/
// M105, plus "ok" and temperature-report replies) into a plain-English
// description.
//
// This is deliberately a lookup table for exactly that fixed vocabulary,
// not a general gcode parser — Remotica never sends arbitrary gcode
// outside of an actual print (and real print streaming isn't built yet,
// see job_manager_tick()'s comment in the backend), so trying to
// understand arbitrary commands here would be both harder and less
// accurate than just covering the ones that can actually show up.
//
// Returns null for anything unrecognized, so callers can fall back to
// showing the raw line instead of a wrong guess.

const AXIS_LABELS = { X: "X", Y: "Y", Z: "Z", E: "the extruder" };

function translateSent(text) {
  if (text === "G91") return "Switch to relative positioning";
  if (text === "G90") return "Switch to absolute positioning";
  if (text === "G28") return "Home all axes";
  if (text === "M105") return "Request a temperature report";

  const move = text.match(/^G1 ([XYZE])(-?[\d.]+)(?: F-?[\d.]+)?$/);
  if (move) {
    const [, axis, deltaText] = move;
    const delta = Number(deltaText);
    const sign = delta > 0 ? "+" : "";
    return `Move ${AXIS_LABELS[axis]} by ${sign}${delta.toFixed(1)}mm`;
  }

  const hotendTemp = text.match(/^M104 S(-?[\d.]+)$/);
  if (hotendTemp) {
    return `Set hotend target temperature to ${Number(hotendTemp[1]).toFixed(0)}°C`;
  }

  const bedTemp = text.match(/^M140 S(-?[\d.]+)$/);
  if (bedTemp) {
    return `Set bed target temperature to ${Number(bedTemp[1]).toFixed(0)}°C`;
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

  return null;
}

export function translateGcodeLine(text, direction) {
  const trimmed = text.trim();
  return direction === "sent"
    ? translateSent(trimmed)
    : translateReceived(trimmed);
}
