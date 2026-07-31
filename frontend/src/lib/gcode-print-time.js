// Slicers embed their own time estimate as a header comment, e.g.
// PrusaSlicer/SuperSlicer/OrcaSlicer:
//   ; estimated printing time (normal mode) = 1h 12m 3s
// Cura:
//   ;TIME:4323
const PRUSA_TIME_RE =
  /;\s*estimated printing time \(normal mode\)\s*=\s*([0-9dhms\s]+)/i;
const CURA_TIME_RE = /;\s*TIME:\s*(\d+)/i;

function parseDurationString(duration) {
  const unitRe = /(\d+)\s*(d|h|m|s)/gi;
  const secondsPerUnit = { d: 86400, h: 3600, m: 60, s: 1 };
  let totalSeconds = 0;
  let match;

  while ((match = unitRe.exec(duration))) {
    totalSeconds += Number(match[1]) * secondsPerUnit[match[2].toLowerCase()];
  }

  return totalSeconds || null;
}

export function extractGcodePrintTimeSeconds(gcodeText) {
  const prusaMatch = gcodeText.match(PRUSA_TIME_RE);
  if (prusaMatch) {
    const seconds = parseDurationString(prusaMatch[1]);
    if (seconds) return seconds;
  }

  const curaMatch = gcodeText.match(CURA_TIME_RE);
  if (curaMatch) return Number(curaMatch[1]);

  return null;
}

export function formatDurationShort(totalSeconds) {
  if (totalSeconds == null) return "Unknown";

  const totalMinutes = Math.round(totalSeconds / 60);
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;

  return hours > 0 ? `${hours}h ${minutes}m` : `${minutes}m`;
}
