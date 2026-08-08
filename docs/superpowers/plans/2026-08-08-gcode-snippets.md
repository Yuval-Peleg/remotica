# G-code Snippets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this task-by-task.

**Goal:** Start/end G-code snippets, edited in Settings, run around every print.

**Architecture:** Snippets live in their own `data/snippets.json` behind `GET`/`POST /api/gcode-snippets`, deliberately outside `PrinterProfile` (whose `source` field is the hardware-unlock gate). `job_manager.c`'s streamer sends them around the file. Validation is frontend-only and advisory.

**Spec:** `docs/superpowers/specs/2026-08-08-gcode-snippets-design.md`

## Global Constraints

- Branch: `gcode-snippets`, off `main`.
- No new C dependencies. `clang-format -i backend/src/*.c` + `cppcheck backend/src/` after C changes; never on `third_party/`.
- `npm run lint` + `npm run format` before frontend commits; 3 pre-existing shadcn warnings are expected.
- **Regression bar: with both snippets empty, a print must behave exactly as it does today.**
- Snippet cap **2048 bytes each**, enforced backend-side.
- End G-code runs **only** when `abort_reason == ABORT_NONE`.
- Progress accounting must not include snippet lines.
- Comments only where the *why* is non-obvious.
- Verify against `./run.sh --sim`; never run `packaging/install.sh` locally.

---

## Task 1: Snippet storage

**Files:** create `backend/src/gcode_snippets.h/.c`; modify `backend/src/main.c`

**Produces:**
```c
#define GCODE_SNIPPET_MAX 2048
typedef struct {
    char start_gcode[GCODE_SNIPPET_MAX];
    char end_gcode[GCODE_SNIPPET_MAX];
    char written_for[64];
} GcodeSnippets;
void gcode_snippets_defaults(GcodeSnippets *s);
void gcode_snippets_load(GcodeSnippets *s, const char *path); /* missing/corrupt -> defaults + one log line */
int  gcode_snippets_save(const GcodeSnippets *s, const char *path); /* 0 ok */
cJSON *gcode_snippets_to_json(const GcodeSnippets *s);
void gcode_snippets_from_json(GcodeSnippets *s, const cJSON *json);
```

- [ ] Write both files, following `printer_profile.c`'s load/save/JSON shape exactly (same `read_string_field` style, same "only overwrite on a valid value" contract).
- [ ] In `main.c`: compose `snippets_path` from `data_dir` (`SNIPPETS_FILENAME "snippets.json"`), load into a `GcodeSnippets` local, add `GcodeSnippets *snippets;` + `const char *snippets_path;` to `AppContext`.
- [ ] Verify: `cd backend && make`; run with `--data-dir /tmp/x`, confirm no crash and no file written yet.
- [ ] Verify corrupt handling: `echo 'not json' > /tmp/x/snippets.json`, restart, confirm one log line and a working backend.
- [ ] `clang-format`, `cppcheck`, commit.

---

## Task 2: The endpoint

**Files:** modify `backend/src/api_handlers.c`

**Consumes:** Task 1's `GcodeSnippets`, `AppContext.snippets`, `AppContext.snippets_path`

- [ ] Add `snippets_handler`: `GET` returns `gcode_snippets_to_json`; `POST` parses the body, **rejects with 413 naming the offending snippet if either exceeds `GCODE_SNIPPET_MAX - 1` bytes**, else copies in, saves, returns the stored value.
- [ ] Register `/api/gcode-snippets`.
- [ ] Verify: `GET` returns three empty fields; `POST` round-trips and persists across a restart; a 3 KB snippet returns 413 with a message naming which one.
- [ ] `clang-format`, `cppcheck`, commit.

---

## Task 3: Run them around the print

**Files:** modify `backend/src/job_manager.c`, `backend/src/job_manager.h`

**Consumes:** Task 1's `GcodeSnippets`

- [ ] Add a `const GcodeSnippets *snippets` parameter to whatever `job_manager_start_print` passes into the streamer args, so the streamer thread has its own copy (**copy the struct, don't hold a pointer into `AppContext`** — the user can save new snippets mid-print and the streaming thread must not see a half-written buffer).
- [ ] Add `static int send_snippet(PrinterDriver *driver, const char *text)`: splits on `\n`, reuses the existing comment/blank stripping the file loop already uses, sends each real line via `send_gcode_line`, returns non-zero on the first failure.
- [ ] Call it for `start_gcode` **before** the file loop; on failure set `abort_reason = ABORT_DRIVER_FAILURE` and skip straight to the existing teardown.
- [ ] Call it for `end_gcode` **only** inside the existing `if (abort_reason == ABORT_NONE)` branch, after progress is set to 100.
- [ ] Do **not** touch `total_lines` or `sent_lines`.
- [ ] Verify with `./run.sh --sim` and a console open: start snippet appears before the file's first line at 0%; end snippet after the last at 100%; cancel mid-print shows the safety sequence and **no** end snippet.
- [ ] `clang-format`, `cppcheck`, commit.

---

## Task 4: Validation

**Files:** create `frontend/src/lib/gcode-validate.js`

**Produces:** `validateGcode(text, { position }) -> [{ line, message }]` where `position` is `"start" | "end"`

- [ ] Implement exactly the six rules in the spec's validation table, structural parsing only.
- [ ] Verify with a node one-liner covering: `G1 X10 Y` (word without value), `M112`, `G28` in end, start without `G28`, end without `M104 S0`, and a clean snippet returning `[]`.
- [ ] Lint, format, commit.

---

## Task 5: Settings card

**Files:** create `frontend/src/components/ui/textarea.jsx` (`npx shadcn@latest add textarea`); modify `frontend/src/lib/api.js`, `frontend/src/pages/Settings.jsx`

**Consumes:** Task 2's endpoint, Task 4's `validateGcode`

- [ ] `api.getSnippets()` / `api.setSnippets({ startGcode, endGcode, writtenFor })`.
- [ ] Third card "Start & end G-code": two monospace textareas, amber warnings under each with line numbers, one Save with the existing "Saved" tick, char counter past 1500, the framing line *these run on every print — test them with the printer in front of you*, and an amber note when `writtenFor` differs from the current printer name.
- [ ] Save sends `writtenFor` = the current `profile.printerName`.
- [ ] Verify in a browser at 1400px and 390px: warnings appear live, saving persists across reload, nothing blocks saving.
- [ ] Lint, format, commit.

---

## Task 6: Blocking printer-change dialog

**Files:** modify `frontend/src/pages/Settings.jsx`

- [ ] In the existing save/apply handler: if either snippet is non-empty **and** the new printer name differs from `writtenFor`, open a `Dialog` instead of applying.
- [ ] Dialog: title `Your start/end G-code was written for {writtenFor}`, body naming the new printer and warning that start G-code which homes or heats for one machine can be wrong on another, buttons `Review G-code` (closes, cancels the switch) and `I've checked — switch anyway` (applies the profile, then saves snippets with the new `writtenFor`).
- [ ] Not dismissible by outside click (`onInteractOutside` prevented).
- [ ] Verify: save a snippet, switch printer → dialog blocks; Review cancels; Switch anyway applies and updates the note.
- [ ] Lint, format, commit.

---

## Task 7: Docs and release

- [ ] `CLAUDE.md`: `gcode_snippets.h/.c`, the new route, why snippets are outside the profile, and the end-only-on-clean-completion rule.
- [ ] `frontend/CLAUDE.md`: the Settings card, `gcode-validate.js`, the blocking dialog.
- [ ] `README.md`: a short **Start & end G-code** subsection under First run — concise and scannable per house style.
- [ ] Full check: `make`, `cppcheck`, `npm run lint`, `npm run build`, `./run.sh --sim` end to end.
- [ ] Merge to `main`, tag `v0.2.0` with a descriptive annotated message, push, confirm the release and its assets.
