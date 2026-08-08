// Advisory checks for the user-written start/end G-code snippets in
// Settings. Warnings only — this NEVER blocks saving.
//
// That's deliberate. Remotica has no idea what commands a particular
// machine supports (custom firmware, exotic macros, vendor extensions),
// so refusing to save anything it doesn't recognise would be Remotica
// asserting knowledge it doesn't have about someone else's printer.
//
// Three of the checks below aren't generic linting — they're the specific
// mistakes this project has already been bitten by: a printer refusing to
// move because it wasn't homed, and an abort path that left the heaters
// on. Worth flagging precisely because they cost real time to diagnose.

// A G-code "word" is a letter followed by a number: G1, X10.5, S-3, E0.
// Anything else on a line is either a comment or something we can't
// vouch for.
const WORD_RE = /^[A-Za-z]-?\d*\.?\d*$/;

function stripComment(line) {
  // ';' starts a comment anywhere; (…) is the other conventional form.
  const semi = line.indexOf(";");
  const text = semi === -1 ? line : line.slice(0, semi);
  return text.replace(/\([^)]*\)/g, "").trim();
}

function commandsIn(text) {
  return text
    .split("\n")
    .map(stripComment)
    .filter(Boolean)
    .map((line) => line.split(/\s+/)[0].toUpperCase());
}

// Returns [{ line, message }], line being 1-based for display. `position`
// is "start" or "end" — several checks only make sense for one of them.
export function validateGcode(text, { position, maxZMm } = {}) {
  const warnings = [];
  const lines = text.split("\n");

  lines.forEach((raw, index) => {
    const line = stripComment(raw);
    if (!line) return;

    const words = line.split(/\s+/);
    for (const word of words) {
      if (WORD_RE.test(word)) {
        // A bare letter with no digits after it — "G1 X10 Y" — is the
        // classic truncation typo, and the printer will reject the line.
        if (!/\d/.test(word)) {
          warnings.push({
            line: index + 1,
            message: `"${word}" has no value after it`,
          });
        }
      } else {
        warnings.push({
          line: index + 1,
          message: `"${word}" isn't recognisable as G-code`,
        });
      }
    }

    if (words[0]?.toUpperCase() === "M112") {
      warnings.push({
        line: index + 1,
        message: "M112 halts the printer until it is reset by hand",
      });
    }
  });

  // Z-height checks. Whether "G1 Z20" means a 20mm height or a 20mm lift
  // depends on the G90/G91 positioning mode in force, so this walks the
  // snippet tracking that mode as it goes.
  //
  // Remotica cannot resolve a relative lift itself: the final Z depends
  // on how tall the print ended up, and the driver doesn't track position
  // from the streamed file (there's no firmware-agnostic way to query it
  // back mid-print — see transport_serial.c). So this warns rather than
  // computing, and never rewrites what the user typed: silently clamping
  // someone's lift would mean their end block isn't what actually runs.
  let mode = null; /* "absolute" | "relative" | null = not set here */
  lines.forEach((raw, index) => {
    const line = stripComment(raw);
    if (!line) return;

    const words = line.split(/\s+/);
    const command = words[0].toUpperCase();
    if (command === "G90") mode = "absolute";
    if (command === "G91") mode = "relative";
    if (command !== "G0" && command !== "G1") return;

    const zWord = words.find((w) => /^[Zz]-?\d*\.?\d+$/.test(w));
    if (!zWord) return;
    const z = Number(zWord.slice(1));

    if (mode === "relative" && z > 0 && position === "end") {
      warnings.push({
        line: index + 1,
        message: `Lifts Z by ${z}mm — on a tall print this can exceed the printer's maximum height. Most firmware clamps it; check yours with M211.`,
      });
    } else if (mode === "absolute" && maxZMm && z > maxZMm) {
      warnings.push({
        line: index + 1,
        message: `Z${z} is above this printer's maximum of ${maxZMm}mm`,
      });
    } else if (mode === null) {
      warnings.push({
        line: index + 1,
        message: `No G90/G91 before this, so whether Z${z} is a height or a lift depends on how the print file ended`,
      });
    }
  });

  const commands = commandsIn(text);
  const hasContent = commands.length > 0;

  if (position === "start" && hasContent && !commands.includes("G28")) {
    warnings.push({
      line: null,
      message:
        "No G28 — the printer may refuse to move until it has been homed",
    });
  }

  if (position === "end" && commands.includes("G28")) {
    warnings.push({
      line: null,
      message: "Homing at the end drags the nozzle across the finished part",
    });
  }

  if (position === "end" && hasContent) {
    // Checked on the full line, not just the command, because it's the
    // S0 that matters — "M104 S200" at the end of a print is the
    // opposite of turning the hotend off.
    const body = text.toUpperCase();
    const hotendOff = /M104\s+S0\b/.test(body) || /M109\s+S0\b/.test(body);
    const bedOff = /M140\s+S0\b/.test(body) || /M190\s+S0\b/.test(body);
    if (!hotendOff || !bedOff) {
      warnings.push({
        line: null,
        message: `Doesn't turn the ${
          !hotendOff && !bedOff
            ? "hotend or bed"
            : !hotendOff
              ? "hotend"
              : "bed"
        } off`,
      });
    }
  }

  return warnings;
}
