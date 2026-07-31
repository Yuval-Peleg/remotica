// Slicers like PrusaSlicer/SuperSlicer/OrcaSlicer embed a preview image
// straight into the .gcode file as commented-out base64, e.g.:
//
//   ; thumbnail begin 220x220 12345
//   ; iVBORw0KGgoAAAANSUhEUgAA...
//   ; thumbnail end
//
// This pulls the largest embedded thumbnail out of that text, if present.
const THUMBNAIL_BLOCK_RE =
  /;\s*thumbnail(?:_JPG|_QOI)?\s+begin\s+(\d+)x(\d+)\s+\d+\r?\n([\s\S]*?);\s*thumbnail(?:_JPG|_QOI)?\s+end/gi;

export function extractGcodeThumbnail(gcodeText) {
  let best = null;
  let match;

  while ((match = THUMBNAIL_BLOCK_RE.exec(gcodeText))) {
    const width = Number(match[1]);
    const height = Number(match[2]);
    const base64 = match[3]
      .split(/\r?\n/)
      .map((line) => line.replace(/^;\s?/, "").trim())
      .join("");

    if (!base64) continue;
    if (!best || width * height > best.width * best.height) {
      best = { width, height, base64 };
    }
  }

  return best ? `data:image/png;base64,${best.base64}` : null;
}
