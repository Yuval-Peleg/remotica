# Start / end G-code snippets

**Date:** 2026-08-08
**Status:** Approved, ready for planning
**Milestone:** v0.2

## Goal

Let the user define G-code that runs before and after every print, edited in
Settings — the thing OctoPrint calls start/end GCODE scripts, and the most
immediately-felt gap for anyone coming from it.

Typical uses: a purge line, a nozzle wipe, parking the head, presenting the
bed, turning off a chamber fan.

## Non-goals

- **Not a macro system.** No named macros, no buttons, no per-file
  overrides, no variables/templating. Two snippets, one printer.
- **Not a G-code editor.** No autocomplete, no formatting, no execution
  preview.
- **Not slicer replacement.** A sliced file carries its own start block;
  these run around it, not instead of it.

## Storage

`data/snippets.json`, served by `GET`/`POST /api/gcode-snippets`.

```json
{
  "startGcode": "G29 ; bed mesh\n",
  "endGcode": "M104 S0\nM140 S0\nG91\nG1 Z10\nG90\nM84\n",
  "writtenFor": "Creality Ender 3 / Ender 3 Pro"
}
```

**Deliberately not in `PrinterProfile`.** The profile carries safety
meaning now — `source: "manual"` is what unlocks the hardware, and
`POST /api/profile` sets it. Two problems if snippets lived there:

1. Saving a macro would go through that endpoint and so silently count as
   confirming the printer — defeating the confirmation gate through a side
   door.
2. Applying a preset replaces the profile's contents, and presets carry no
   G-code, so choosing a printer from the dropdown would wipe the user's
   snippets.

Both are fixable with merge rules, but merge rules are exactly what gets
forgotten later.

`writtenFor` is the profile's `printerName` at the moment of saving. It
exists only to drive the printer-change warning below.

**Limits:** 2 KB per snippet, enforced backend-side (413 if exceeded). A
snippet is plain text; empty string means "nothing to run".

## Execution

In `job_manager.c`'s `streamer_thread_main`:

1. Send each non-blank, non-comment line of `startGcode`.
2. Stream the file as today.
3. On clean completion only, send each line of `endGcode`.

**Before the file, not after its header** — a sliced file carries its own
start block (heat, home, prime), and the user's snippet should precede it.

Snippet lines go through the same `send_gcode_line()` as file lines, so on
real hardware they get line numbers and checksums like everything else. A
snippet line that fails is treated exactly as a failing file line: set
`abort_reason`, run the safety sequence, end the job.

**Progress ignores snippet lines.** `total_lines` stays the file's own
count, so progress sits at 0% during start G-code and reaches 100% before
end G-code runs. The file is what's being printed; padding the denominator
with lines the user didn't slice would make the estimate worse.

**End G-code does not run on cancel or failure** — only when
`abort_reason == ABORT_NONE`. A cancel keeps today's fixed sequence
(heaters off, fan off, 5 mm Z lift, steppers released). This is the path
that already failed once by leaving heaters at temperature for 20+
minutes; it stays short and predictable rather than gaining arbitrary
user commands at the worst possible moment.

## Validation — advisory, never blocking

New `frontend/src/lib/gcode-validate.js`, run as the user types and again
before save. Returns a list of `{ line, severity, message }`. **Saving is
never prevented** — Remotica doesn't know every machine's exotic commands
and shouldn't pretend to.

| Condition | Message |
| --- | --- |
| A word letter with no value (`G1 X10 Y`) | `Line N: "Y" has no value` |
| Unparseable line (not `;`-comment, not `LETTER<number>` words) | `Line N: not recognisable as G-code` |
| `M112` anywhere | `M112 halts the printer until it is reset` |
| `G28` in end G-code | `Homing at the end drags the nozzle across the finished part` |
| No `G28` in start G-code (and start G-code non-empty) | `No G28 — the printer may refuse to move until homed` |
| End G-code non-empty without `M104 S0` and `M140 S0` | `Heaters are not turned off at the end` |

The last three are not generic linting: each is a mistake this project has
already been bitten by (the homing gate, and the abort sequence leaving
heaters on).

Structural parsing only — no attempt to understand semantics. Deliberately
separate from `gcode-translate.js`, which is a description lookup, not a
validator; they share no code.

## Printer-change warning — blocking

When "Use this printer" would change the profile's `printerName`, and
either snippet is non-empty, and `writtenFor` differs from the new name:

> **Your start/end G-code was written for _{writtenFor}_.**
> You're switching to _{newName}_. Start G-code that homes or heats for
> one machine can be wrong on another.
> `[Review G-code]` `[I've checked — switch anyway]`

Modal, not dismissible by clicking outside — the user picks one. "Review
G-code" cancels the switch and scrolls to the snippets card. "Switch
anyway" applies the profile and updates `writtenFor` to the new name.

Considered and rejected: refusing to *start a print* until snippets are
re-confirmed. That raises the objection at the wrong moment — "why won't
it print" an hour later is worse than a question asked exactly when the
user caused it.

## UI

A third card in `Settings.jsx`, **"Start & end G-code"**, below the
existing two:

- Two monospace `Textarea`s (needs shadcn's `textarea`, not yet in the
  project), labelled "Before every print" / "After every print".
- Warnings listed under each box, amber, with line numbers.
- One **Save** button for both, with the existing "Saved" tick pattern.
- Character counter shown only past 1.5 KB.
- One line of framing on the card: *these run on every print — test them
  with the printer in front of you.*
- When `writtenFor` differs from the current printer name, a persistent
  amber note: *written for {writtenFor}*.

## Error handling

- Snippet over 2 KB → 413, message names which snippet.
- `snippets.json` missing or corrupt → treated as two empty snippets, one
  log line. Never fatal; a bad macro file must not stop the printer being
  usable.
- Snippet line rejected by the printer → identical to a file line
  failing: abort + safety sequence, and the console shows which line.

## Verification

1. Empty snippets → a print behaves exactly as today (regression bar).
2. Start snippet of `M117 hello` → appears in the console before the
   file's first line, and progress is still 0% at that point.
3. End snippet → appears after the last file line; progress already 100%.
4. Cancel mid-print → end snippet does **not** appear; the fixed safety
   sequence does.
5. A deliberately-failing snippet line → print aborts, safety sequence
   runs, console shows the offending line.
6. Each validation rule fires on a crafted input and none blocks saving.
7. Save snippets, then switch printer in Settings → dialog appears and
   blocks; "Review" cancels the switch; "Switch anyway" applies it and
   updates `writtenFor`.
8. Corrupt `snippets.json` by hand → backend starts, prints work, one log
   line.

## Critical files

- `backend/src/gcode_snippets.h/.c` — new: load/save/validate size
- `backend/src/job_manager.c` — send around the file stream
- `backend/src/api_handlers.c` — `GET`/`POST /api/gcode-snippets`
- `backend/src/main.c` — path wiring, `--data-dir` aware
- `frontend/src/lib/gcode-validate.js` — new
- `frontend/src/pages/Settings.jsx` — the card and the change dialog
- `frontend/src/components/ui/textarea.jsx` — new (shadcn)
- `frontend/src/lib/api.js`
