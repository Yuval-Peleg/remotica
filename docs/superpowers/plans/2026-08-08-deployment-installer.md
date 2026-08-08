# Deployment & Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Remotica installable on the Ubuntu PC wired to the printer with one downloaded command — no C toolchain, no Node, no repo checkout — running as a systemd service whose boot behaviour is toggleable from the app itself.

**Architecture:** The backend gains two flags (`--web-root`, `--data-dir`) that let it serve the pre-built frontend and store data outside its own directory, which turns it into a single deployable process. GitHub Actions builds that binary plus the frontend into a Release tarball. A shell installer places it under `/usr/local`, runs it as an unprivileged `remotica` user, and grants that user exactly three permitted `systemctl` command lines via `/etc/sudoers.d/` so a new `/api/system` endpoint can toggle boot-start without the network-facing server ever holding root.

**Tech Stack:** C99 + civetweb + cJSON (backend), React 19 + Vite + Tailwind v4 + shadcn/ui (frontend), POSIX shell (installer), systemd, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-08-08-deployment-installer-design.md`

## Global Constraints

- **Branch:** all work on `deployment-installer` (already created, branched from `motion-polish`).
- **No new C dependencies.** If one seems needed, stop and ask — every dependency must be MIT-compatible and vendored under `backend/third_party/`.
- **Never run `git config`.** The repo's git identity was chosen deliberately.
- **`clang-format -i backend/src/*.c` after C changes.** Never run it on `backend/third_party/`.
- **`cppcheck backend/src/`** must report no new findings.
- **Existing behaviour must not change when the new flags are absent.** `./run.sh` and `./run.sh --sim` must work exactly as they do today, with relative `data/` and the Vite dev server. This is the regression bar for every backend task.
- **Comments only where the *why* is non-obvious** (repo convention) — not narration of what the code does.
- **Frontend:** `npm run lint` and `npm run format` before every frontend commit. Three pre-existing fast-refresh warnings in `button.jsx`/`badge.jsx`/`tabs.jsx` are expected and not regressions.
- **This project has no automated C or JS test suite.** Verification steps below are real commands with expected output, run manually. Do not scaffold a test framework — that is scope the user did not ask for. The one existing harness, `backend/tools/fake_marlin_test.py`, covers serial protocol only and is untouched by this work.
- **Service unit name is `remotica.service` everywhere.** The sudoers rules are literal strings; any rename breaks them silently.
- **Port is `8080`** for the installed service (the backend's existing default).

---

## File Structure

**Backend — modified**
- `backend/src/main.c` — parse `--web-root`/`--data-dir`, compose data paths, pass `document_root` to civetweb, register the SPA fallback, record start time
- `backend/src/api_handlers.h` — `AppContext` gains `web_root`, `data_dir`, `serial_device`, `started_at`
- `backend/src/api_handlers.c` — `GET /api/system`, `POST /api/system/boot-start`
- `backend/Makefile` — version define

**Backend — created**
- `backend/src/system_info.h` / `.c` — everything that shells out to `systemctl` or reads OS-level facts (uptime, free disk). Kept separate from `api_handlers.c` because that file is already ~830 lines and this is a distinct responsibility: talking to the host OS rather than to the printer.

**Packaging — created**
- `packaging/remotica.service`
- `packaging/install.sh` — also serves as the uninstaller when invoked as `remotica-uninstall`
- `packaging/sudoers.remotica`
- `.github/workflows/release.yml`

**Frontend — created/modified**
- `frontend/src/pages/System.jsx` — replaces the placeholder
- `frontend/src/lib/api.js` — `getSystem()`, `setBootStart()`
- `frontend/src/App.jsx` — route the real page

**Docs — modified**
- `README.md`, `ROADMAP.md`, `CLAUDE.md`, `frontend/CLAUDE.md`

---

## Task 1: `--data-dir` flag

Lets the backend store the profile and uploads somewhere other than `./data`, which is what allows it to run as a service from `/`.

**Files:**
- Modify: `backend/src/main.c` (the `PROFILE_PATH`/`UPLOADS_DIR` defines at lines 66-67, and the arg loop at lines 231-237)

**Interfaces:**
- Consumes: nothing
- Produces: `main()` local `char profile_path[512]` and `char uploads_dir[512]`, passed to `AppContext.profile_path` / `.uploads_dir` exactly as the old defines were

- [ ] **Step 1: Replace the compile-time path defines with a base-directory default**

In `backend/src/main.c`, replace lines 66-67 (`#define PROFILE_PATH` / `#define UPLOADS_DIR`) with:

```c
/* Where the profile and uploaded gcode live, relative to --data-dir.
 * --data-dir itself defaults to "data", which keeps the historical
 * behaviour of running from backend/ with a relative data directory —
 * run.sh and every dev workflow depend on that and pass no flag. An
 * installed service passes an absolute path instead, because it runs as
 * an unprivileged user whose working directory is not the repo. */
#define DEFAULT_DATA_DIR "data"
#define PROFILE_FILENAME "profile.json"
#define UPLOADS_SUBDIR "uploads"
```

- [ ] **Step 2: Parse the flag and compose the paths**

In `main()`, alongside the existing `serial_device` / `listen_port` locals, add:

```c
const char *data_dir = DEFAULT_DATA_DIR;
```

Add to the arg loop (after the `--port` branch):

```c
} else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
    data_dir = argv[i + 1];
    i++;
}
```

After the loop, compose both paths:

```c
char profile_path[512];
char uploads_dir[512];
snprintf(profile_path, sizeof(profile_path), "%s/%s", data_dir, PROFILE_FILENAME);
snprintf(uploads_dir, sizeof(uploads_dir), "%s/%s", data_dir, UPLOADS_SUBDIR);
```

Add `#include <stdio.h>` if not already present (it is).

- [ ] **Step 3: Replace every use of the old defines**

`grep -n "PROFILE_PATH\|UPLOADS_DIR" backend/src/main.c` and replace each with the new locals. There are uses in the profile load, the `AppContext` initialiser, `job_manager_init`, and `try_auto_detect_profile`'s call site. Every one becomes `profile_path` / `uploads_dir`.

- [ ] **Step 4: Verify the default is unchanged**

```bash
cd backend && make 2>&1 | tail -5
grep -rn "PROFILE_PATH\|UPLOADS_DIR" src/    # expect: no matches
```

Then, from `backend/`:

```bash
rm -rf /tmp/rm-test && ./build/remotica-backend --port 8099 &
sleep 2 && ls data/ && curl -s localhost:8099/api/profile | head -c 100
kill %1
```

Expected: `data/` still used, profile returned.

- [ ] **Step 5: Verify the flag works**

```bash
cd backend && ./build/remotica-backend --port 8099 --data-dir /tmp/rm-test &
sleep 2 && curl -s -X POST localhost:8099/api/profile \
  -H 'Content-Type: application/json' -d '{"bedWidthMm":220}' >/dev/null
ls /tmp/rm-test/
kill %1
```

Expected: `/tmp/rm-test/profile.json` exists. Uploads dir is created lazily by `job_manager.c`'s `mkdir` chain — confirm it handles an absolute multi-level path by uploading a file:

```bash
cd backend && ./build/remotica-backend --port 8099 --data-dir /tmp/rm-test2 &
sleep 2 && printf 'G28\n' > /tmp/t.gcode
curl -s -X POST 'localhost:8099/api/upload?filename=t.gcode' --data-binary @/tmp/t.gcode
ls /tmp/rm-test2/uploads/
kill %1
```

Expected: `t.gcode` listed. If `job_manager.c`'s mkdir chain only creates two levels and the absolute path is deeper, fix it there to create each path component in turn.

- [ ] **Step 6: Format, check, commit**

```bash
clang-format -i backend/src/main.c && cppcheck backend/src/ 2>&1 | tail -20
git add backend/src/main.c backend/src/job_manager.c
git commit -m "Let the backend store its data outside its own directory

--data-dir defaults to \"data\", so running from backend/ behaves
exactly as before and run.sh needs no change. An installed service
runs as an unprivileged user from /, where a relative path can't work."
```

---

## Task 2: Serve the built frontend (`--web-root` + SPA fallback)

**Files:**
- Modify: `backend/src/main.c` (civetweb options array ~line 392, arg loop, plus a new fallback handler)
- Modify: `backend/Makefile`

**Interfaces:**
- Consumes: Task 1's arg-loop pattern
- Produces: `AppContext` is not yet touched; `web_root` stays a `main()` local this task and is added to `AppContext` in Task 3

- [ ] **Step 1: Add the version define to the Makefile**

In `backend/Makefile`, after the `CFLAGS :=` line:

```make
# Baked in at build time so /api/system can report which build is running.
# `git describe` fails in a tarball with no .git, so this falls back to
# "dev" rather than breaking the build.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CFLAGS += -DREMOTICA_VERSION=\"$(VERSION)\"
```

In `main.c`, near the other defines:

```c
#ifndef REMOTICA_VERSION
#define REMOTICA_VERSION "dev"
#endif
```

- [ ] **Step 2: Parse `--web-root` and pass it to civetweb**

Add the local and arg branch, same shape as Task 1:

```c
const char *web_root = NULL;
```
```c
} else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
    web_root = argv[i + 1];
    i++;
}
```

The options array is currently a fixed initialiser. Replace it with one built conditionally, because civetweb must not receive a `document_root` key at all when there's no web root:

```c
/* Built at runtime rather than as a fixed initialiser because
 * document_root must be absent entirely (not empty) when running
 * without --web-root — otherwise civetweb serves the process's
 * current directory, which in a dev checkout is backend/ and would
 * expose source and uploaded gcode over HTTP. */
const char *options[9];
size_t opt = 0;
options[opt++] = "listening_ports";
options[opt++] = listen_port;
options[opt++] = "num_threads";
options[opt++] = "8";
options[opt++] = "request_timeout_ms";
options[opt++] = "3000";
if (web_root != NULL) {
    options[opt++] = "document_root";
    options[opt++] = web_root;
}
options[opt] = NULL;
```

Keep the existing long explanatory comment above it.

- [ ] **Step 3: Warn, don't die, when the web root is missing**

After parsing, before `mg_start`:

```c
if (web_root != NULL && access(web_root, R_OK) != 0) {
    fprintf(stderr,
            "Warning: --web-root %s is not readable. The API will still work, "
            "but the dashboard will not load.\n",
            web_root);
}
```

A broken frontend install must not take printer control down with it.

- [ ] **Step 4: Register the SPA fallback**

Add above `main()`:

```c
/* React Router owns /settings and /about — they are not files on disk.
 * A browser asking the server for one directly (refresh, bookmark, a
 * link opened on a phone) would otherwise get civetweb's 404. Serving
 * index.html instead lets the router take over on the client.
 *
 * Registered on "/", so civetweb only reaches it after every /api/*
 * handler and every real file under document_root have been tried. */
static int spa_fallback_handler(struct mg_connection *conn, void *cbdata) {
    const char *root = (const char *)cbdata;
    const struct mg_request_info *req = mg_get_request_info(conn);

    if (strncmp(req->local_uri, "/api/", 5) == 0) {
        return 0;
    }

    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", root);
    mg_send_file(conn, index_path);
    return 1;
}
```

Register it after `api_handlers_register_all(...)` and every other route, only when serving a frontend:

```c
if (web_root != NULL) {
    mg_set_request_handler(ctx, "/", spa_fallback_handler, (void *)web_root);
}
```

- [ ] **Step 5: Build a frontend and verify all three cases**

```bash
cd frontend && npm run build && cd ../backend && make
./build/remotica-backend --port 8099 --web-root ../frontend/dist &
sleep 2
curl -s -o /dev/null -w '%{http_code} ' localhost:8099/              # 200
curl -s -o /dev/null -w '%{http_code} ' localhost:8099/settings      # 200
curl -s -o /dev/null -w '%{http_code} ' localhost:8099/about         # 200
curl -s -o /dev/null -w '%{http_code} ' localhost:8099/api/state     # 200
curl -s localhost:8099/settings | grep -c '<div id="root"'           # 1
curl -s localhost:8099/api/state | head -c 60                        # JSON, not HTML
kill %1
```

The two checks that matter most: `/settings` returning 200 with the app shell (proves the fallback works) and `/api/state` returning JSON (proves the fallback did **not** swallow API routes).

- [ ] **Step 6: Verify dev mode is untouched**

```bash
cd backend && ./build/remotica-backend --port 8099 &
sleep 2 && curl -s -o /dev/null -w '%{http_code}\n' localhost:8099/    # 404, NOT a directory listing
kill %1
./run.sh --sim   # loads at :5173, all routes work; Ctrl+C
```

A directory listing at `/` means `document_root` leaked in — go back to Step 2.

- [ ] **Step 7: Format, check, commit**

```bash
clang-format -i backend/src/main.c && cppcheck backend/src/ 2>&1 | tail -20
git add backend/src/main.c backend/Makefile
git commit -m "Serve the built frontend from the backend

--web-root turns the two-process dev setup into one deployable
process. Includes a catch-all that serves index.html for client-side
routes, since /settings and /about aren't files and a refresh on
either would otherwise 404. The catch-all declines /api/* explicitly."
```

---

## Task 3: `GET /api/system`

**Files:**
- Create: `backend/src/system_info.h`, `backend/src/system_info.c`
- Modify: `backend/src/api_handlers.h` (`AppContext`), `backend/src/api_handlers.c`, `backend/src/main.c`

**Interfaces:**
- Consumes: `--web-root`/`--data-dir` locals from Tasks 1-2
- Produces:
  ```c
  /* system_info.h */
  #define REMOTICA_UNIT_PATH "/etc/systemd/system/remotica.service"
  int       system_info_is_managed(void);             /* 1 if the unit file exists */
  int       system_info_boot_start_enabled(void);     /* 1 enabled, 0 disabled, -1 unknown */
  int       system_info_set_boot_start(int enable, char *err, size_t err_size); /* 1 ok, 0 fail */
  long long system_info_free_bytes(const char *path); /* -1 on failure */
  ```
  and `AppContext` gains `const char *web_root; const char *data_dir; const char *serial_device; time_t started_at;`

- [ ] **Step 1: Write `system_info.h`**

```c
#ifndef REMOTICA_SYSTEM_INFO_H
#define REMOTICA_SYSTEM_INFO_H

/*
 * system_info.h
 * ==============
 * Facts about the host OS rather than the printer: is Remotica running
 * as an installed systemd service, will it come back after a reboot,
 * and how much room is left for uploads.
 *
 * Separate from api_handlers.c on purpose — that file is about the
 * printer, this one is the only place that shells out to systemctl, so
 * the privileged surface is one small file you can read in full.
 */

#define REMOTICA_UNIT_PATH "/etc/systemd/system/remotica.service"

/* 1 when the systemd unit file exists, meaning this is an installed
 * service whose boot behaviour can be toggled. 0 in a dev checkout. */
int system_info_is_managed(void);

/* 1 enabled, 0 disabled, -1 if it couldn't be determined. */
int system_info_boot_start_enabled(void);

/* 1 on success, 0 on failure. `enable` is a boolean. */
int system_info_set_boot_start(int enable, char *err, size_t err_size);

/* Free bytes on the filesystem holding `path`, or -1. */
long long system_info_free_bytes(const char *path);

#endif /* REMOTICA_SYSTEM_INFO_H */
```

Add `#include <stddef.h>` for `size_t`.

- [ ] **Step 2: Write `system_info.c`**

```c
#include "system_info.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

int system_info_is_managed(void) {
    struct stat st;
    return stat(REMOTICA_UNIT_PATH, &st) == 0;
}

/* Runs one of a fixed set of command strings and returns its exit status,
 * with up to err_size-1 bytes of its output copied into err.
 *
 * SAFETY: every caller passes a compile-time constant. Nothing from the
 * network is ever interpolated into `cmd` — the boot-start endpoint
 * selects between two fixed strings on a boolean rather than building a
 * command from a request. Keep it that way; this is the one place in the
 * backend that can run anything as root. */
static int run_fixed_command(const char *cmd, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) {
        return -1;
    }

    if (err != NULL && err_size > 0) {
        size_t used = 0;
        int c;
        while ((c = fgetc(pipe)) != EOF && used < err_size - 1) {
            err[used++] = (char)c;
        }
        err[used] = '\0';
    }

    int status = pclose(pipe);
    return status;
}

int system_info_boot_start_enabled(void) {
    if (!system_info_is_managed()) {
        return -1;
    }

    char out[64];
    run_fixed_command("sudo /usr/bin/systemctl is-enabled remotica.service 2>&1", out,
                      sizeof(out));

    /* is-enabled exits non-zero for "disabled", so the exit status alone
     * can't distinguish "disabled" from "the command failed" — the word
     * it prints is the actual answer. */
    if (strncmp(out, "enabled", 7) == 0) {
        return 1;
    }
    if (strncmp(out, "disabled", 8) == 0) {
        return 0;
    }
    return -1;
}

int system_info_set_boot_start(int enable, char *err, size_t err_size) {
    const char *cmd = enable ? "sudo /usr/bin/systemctl enable remotica.service 2>&1"
                             : "sudo /usr/bin/systemctl disable remotica.service 2>&1";
    return run_fixed_command(cmd, err, err_size) == 0;
}

long long system_info_free_bytes(const char *path) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        return -1;
    }
    return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}
```

- [ ] **Step 3: Extend `AppContext`**

In `backend/src/api_handlers.h`, add to the struct, with a comment for the non-obvious one:

```c
    const char *web_root;      /* NULL when not serving a built frontend */
    const char *data_dir;      /* what free-space is reported against */
    const char *serial_device; /* NULL means the simulator */
    time_t started_at;         /* for uptime */
```

Add `#include <time.h>`.

In `main.c`, set all four in the `AppContext` initialiser at line ~405, recording `time(NULL)` into a local at the top of `main()`.

- [ ] **Step 4: Write the handler**

In `api_handlers.c`, add `#include "system_info.h"` and, next to the other handlers:

```c
/* ---------------------------------------------------------------------
 * GET /api/system
 * --------------------------------------------------------------------- */

static int system_handler(struct mg_connection *conn, void *cbdata) {
    AppContext *ctx = (AppContext *)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "GET") != 0) {
        mg_send_http_error(conn, 405, "Only GET is supported here");
        return 1;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "version", REMOTICA_VERSION);
    cJSON_AddNumberToObject(json, "uptimeSeconds", (double)(time(NULL) - ctx->started_at));
    cJSON_AddStringToObject(json, "dataDir", ctx->data_dir);
    cJSON_AddNumberToObject(json, "dataFreeBytes",
                            (double)system_info_free_bytes(ctx->data_dir));
    cJSON_AddStringToObject(json, "serialPort",
                            ctx->serial_device ? ctx->serial_device : "simulator");

    int managed = system_info_is_managed();
    cJSON_AddBoolToObject(json, "managed", managed);
    if (managed) {
        int enabled = system_info_boot_start_enabled();
        if (enabled >= 0) {
            cJSON_AddBoolToObject(json, "bootStartEnabled", enabled);
        }
    }

    send_json_and_delete(conn, json);
    return 1;
}
```

`REMOTICA_VERSION` is defined in `main.c` today — move that `#ifndef` block into `system_info.h` so both files see it, and delete it from `main.c`.

Register it in `api_handlers_register_all`:

```c
mg_set_request_handler(ctx, "/api/system", system_handler, app_context);
```

- [ ] **Step 5: Verify**

```bash
cd backend && make && ./build/remotica-backend --port 8099 --data-dir /tmp/rm-test &
sleep 2 && curl -s localhost:8099/api/system | python3 -m json.tool
kill %1
```

Expected on a dev machine: `"managed": false`, **no** `bootStartEnabled` key, `"serialPort": "simulator"`, a plausible `dataFreeBytes`, `"version": "dev"` or a `git describe` string.

- [ ] **Step 6: Format, check, commit**

```bash
clang-format -i backend/src/*.c && cppcheck backend/src/ 2>&1 | tail -20
git add backend/src/system_info.h backend/src/system_info.c backend/src/api_handlers.h backend/src/api_handlers.c backend/src/main.c backend/Makefile
git commit -m "Report host-level status over GET /api/system

Version, uptime, data directory and free space, serial port, and
whether this is an installed service whose boot behaviour can be
toggled. Everything that shells out to systemctl lives in system_info.c
so the privileged surface is one short file."
```

---

## Task 4: `POST /api/system/boot-start`

**Files:**
- Modify: `backend/src/api_handlers.c`

**Interfaces:**
- Consumes: `system_info_set_boot_start(int, char *, size_t)` from Task 3
- Produces: nothing further

- [ ] **Step 1: Write the handler**

```c
/* ---------------------------------------------------------------------
 * POST /api/system/boot-start   {"enabled": true|false}
 * --------------------------------------------------------------------- */

static int boot_start_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *req_info = mg_get_request_info(conn);

    if (strcmp(req_info->request_method, "POST") != 0) {
        mg_send_http_error(conn, 405, "Only POST is supported here");
        return 1;
    }

    if (!system_info_is_managed()) {
        mg_send_http_error(conn, 409,
                           "Not running as an installed service, so there is no "
                           "boot behaviour to change");
        return 1;
    }

    cJSON *json = read_json_body(conn);
    if (json == NULL) {
        return 1;
    }

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        mg_send_http_error(conn, 400, "Expected {\"enabled\": true|false}");
        return 1;
    }
    int want_enabled = cJSON_IsTrue(enabled);
    cJSON_Delete(json);

    /* systemctl's own stderr is forwarded to the client deliberately: a
     * sudoers rule that doesn't match (a renamed unit, a botched install)
     * fails in a way that is otherwise completely invisible from the UI. */
    char err[256];
    if (!system_info_set_boot_start(want_enabled, err, sizeof(err))) {
        mg_send_http_error(conn, 500, "systemctl failed: %s", err[0] ? err : "no output");
        return 1;
    }

    send_ok(conn);
    return 1;
}
```

Register it **after** `/api/system` — civetweb matches longest prefix, but register in this order to keep it obvious:

```c
mg_set_request_handler(ctx, "/api/system/boot-start", boot_start_handler, app_context);
```

- [ ] **Step 2: Verify the guard on a dev machine**

```bash
cd backend && make && ./build/remotica-backend --port 8099 &
sleep 2
curl -s -o /dev/null -w '%{http_code}\n' -X POST localhost:8099/api/system/boot-start \
     -H 'Content-Type: application/json' -d '{"enabled":true}'   # 409
curl -s -o /dev/null -w '%{http_code}\n' -X GET localhost:8099/api/system/boot-start  # 405
kill %1
```

409 is the correct result here — there is no unit file on the dev machine. The success path can only be tested after Task 6 installs one; that is Task 6's verification, not this one's.

- [ ] **Step 3: Confirm `/api/system` still routes correctly**

```bash
cd backend && ./build/remotica-backend --port 8099 & sleep 2
curl -s localhost:8099/api/system | head -c 40   # still JSON, not a 405 from boot-start
kill %1
```

- [ ] **Step 4: Format, check, commit**

```bash
clang-format -i backend/src/api_handlers.c && cppcheck backend/src/ 2>&1 | tail -20
git add backend/src/api_handlers.c
git commit -m "Add POST /api/system/boot-start

409s when there's no unit file to act on, and forwards systemctl's
own stderr on failure — a sudoers rule that doesn't match is otherwise
invisible from the UI."
```

---

## Task 5: The System page

**Files:**
- Create: `frontend/src/pages/System.jsx`
- Modify: `frontend/src/lib/api.js`, `frontend/src/App.jsx`

**Interfaces:**
- Consumes: `GET /api/system`, `POST /api/system/boot-start` from Tasks 3-4
- Produces: `api.getSystem()`, `api.setBootStart(enabled)`; `<System />` exported from `pages/System.jsx`

- [ ] **Step 1: Add the API calls**

In `frontend/src/lib/api.js`, following the existing wrapper style exactly:

```js
getSystem: () => request("/api/system"),
setBootStart: (enabled) =>
  request("/api/system/boot-start", {
    method: "POST",
    body: JSON.stringify({ enabled }),
  }),
```

Match the surrounding code's actual helper name and signature — read the file first; do not assume `request` is what it's called.

- [ ] **Step 2: Check whether a Switch component exists**

```bash
ls frontend/src/components/ui/switch.jsx
```

If missing: `cd frontend && npx shadcn@latest add switch`.

- [ ] **Step 3: Write the page**

`frontend/src/pages/System.jsx`. Requirements, following the conventions in `About.jsx` and `Settings.jsx`:

- Fetch on mount via `api.getSystem()`; hold `{ info, error, saving }` state.
- Poll every 5s so uptime advances, using `setInterval` cleared on unmount.
- A status card listing: Version, Uptime (formatted `2d 4h 11m`, not raw seconds), Serial port, Data directory, Free space (formatted GB, not raw bytes).
- A "Start on boot" row with a `Switch`:
  - `info.managed === false` → switch `disabled`, with muted helper text: *"Remotica is running from a source checkout, not as an installed service. Install it with the script in the README to manage boot behaviour from here."*
  - `info.managed === true` → switch reflects `info.bootStartEnabled`; toggling calls `api.setBootStart(next)`, disables the switch while in flight, then re-fetches.
- On failure, `toast.error(...)` from `sonner` (the established pattern — see `use-printer-state.js`) carrying the server's message, and revert the switch to its previous value.
- Use `motion-stagger` on the card container and `animate-rise` on the header, matching `About.jsx`.
- No new colors or spacing scales — reuse the existing tokens.

- [ ] **Step 4: Route it**

In `App.jsx`, replace `<Route path="/system" element={<PlaceholderPage title="System" />} />` with `<Route path="/system" element={<System />} />` and add the import. If `PlaceholderPage` then has no remaining users, delete it.

- [ ] **Step 5: Verify**

```bash
cd backend && ./build/remotica-backend --port 8080 & cd ../frontend && npm run dev
```

Open `/system`. Expect: real version/uptime/free space, uptime ticking, and the boot switch **disabled** with the source-checkout explanation. Toggling it should be impossible; if it is possible, the `managed` check is wrong.

- [ ] **Step 6: Lint, format, commit**

```bash
cd frontend && npm run lint && npm run format
git add frontend/src/pages/System.jsx frontend/src/lib/api.js frontend/src/App.jsx frontend/src/components/ui/switch.jsx
git commit -m "Make the System page real

Version, uptime, serial port, data directory and free space, plus the
start-on-boot switch. In a source checkout there is no service to
toggle, so the switch is disabled with an explanation rather than
being a control that silently fails."
```

---

## Task 6: Packaging — service unit, installer, uninstaller

**Files:**
- Create: `packaging/remotica.service`, `packaging/sudoers.remotica`, `packaging/install.sh`

**Interfaces:**
- Consumes: the `--web-root`/`--data-dir` flags from Tasks 1-2
- Produces: the installed layout every later task and the docs refer to

- [ ] **Step 1: Write `packaging/remotica.service`**

```ini
[Unit]
Description=Remotica 3D printer dashboard
After=network.target

[Service]
Type=simple
User=remotica
Group=remotica
ExecStart=/usr/local/bin/remotica --serial auto --web-root /usr/local/share/remotica/web --data-dir /var/lib/remotica
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`--serial auto` rather than a fixed path so a replug that lands the printer on a different `/dev/ttyACM*` still reconnects. `Restart=on-failure` covers a crash and nothing else — a missing printer is not a failure, the backend already starts fine without one and retries in the background.

- [ ] **Step 2: Write `packaging/sudoers.remotica`**

```
remotica ALL=(root) NOPASSWD: /usr/bin/systemctl enable remotica.service, /usr/bin/systemctl disable remotica.service, /usr/bin/systemctl is-enabled remotica.service
```

One line, three exact command lines, absolute paths, no wildcards.

- [ ] **Step 3: Write `packaging/install.sh`**

POSIX `sh`, `set -eu`. It must:

1. Require root (`[ "$(id -u)" = 0 ]`), else print `Re-run with sudo` and exit 1.
2. Detect mode: if invoked as `remotica-uninstall` (`basename "$0"`) or given `--uninstall`, run the uninstall path.
3. Accept `--no-interactive` and `--purge`.
4. **Install path:**
   - `uname -m` → `x86_64` or `aarch64`; anything else exits with the detected value named.
   - Download `remotica-linux-$ARCH.tar.gz` and `SHA256SUMS` from the latest Release into a `mktemp -d`, cleaned up via `trap`.
   - `sha256sum -c` filtered to the one file. **Abort before extracting on mismatch.**
   - `useradd --system --no-create-home --shell /usr/sbin/nologin remotica` guarded by `id -u remotica >/dev/null 2>&1 ||`.
   - Install `remotica` → `/usr/local/bin/`, itself → `/usr/local/bin/remotica-uninstall`, `web/` → `/usr/local/share/remotica/web` (delete the old directory first so a removed asset doesn't linger).
   - `install -d -o remotica -g remotica /var/lib/remotica` — must **not** touch it if it already exists beyond fixing ownership, so an upgrade preserves uploads and the profile.
   - udev rule → `/etc/udev/rules.d/`, then `udevadm control --reload-rules && udevadm trigger`.
   - Sudoers: write to a temp file, `visudo -c -f "$tmp"` — **abort without installing on failure**, since a malformed sudoers file can lock the machine out of `sudo` entirely — then `install -m 0440 "$tmp" /etc/sudoers.d/remotica`.
   - Unit file → `/etc/systemd/system/`, then `systemctl daemon-reload`.
   - Boot question, only if `[ -t 0 ]` and not `--no-interactive`: `Start Remotica automatically when this PC boots? [Y/n]`. On an upgrade where the unit is already enabled, do not ask — preserve the setting.
   - `--no-interactive` defaults boot-start to **off**, so an unattended install never silently arranges for something to auto-start.
   - `systemctl restart remotica`, then wait up to 10s for `curl -s localhost:8080/api/state`. On failure print `journalctl -u remotica -n 50 --no-pager` rather than only "failed".
   - Print the LAN URL from `hostname -I | awk '{print $1}'`, port 8080, and this warning verbatim:
     > **Anyone on your network who opens this URL can control your printer. Remotica has no login.**
5. **Uninstall path**, in this order, each step skipped without failing if already absent:
   `systemctl stop remotica` → `systemctl disable remotica` → remove the unit file → `systemctl daemon-reload` → remove `/etc/sudoers.d/remotica` → remove the udev rule + `udevadm control --reload-rules` → remove `/usr/local/share/remotica` → `userdel remotica` → remove `/usr/local/bin/remotica` → remove itself last.
   - `/var/lib/remotica` is **kept** unless `--purge`. Print what was left and the exact `rm -rf` command to remove it.
   - `--purge` with a terminal attached confirms first, naming the directory.

- [ ] **Step 4: Shell-check it**

```bash
sh -n packaging/install.sh && echo "syntax ok"
command -v shellcheck >/dev/null && shellcheck packaging/install.sh
```

If `shellcheck` isn't installed, skip it — do not `apt-get install` anything without asking (standing user preference).

- [ ] **Step 5: Dry-run the sudoers validation**

```bash
visudo -c -f packaging/sudoers.remotica && echo "sudoers valid"
```

Expected: `parsed OK`. This must pass before the file is ever shipped.

- [ ] **Step 6: Commit**

```bash
git add packaging/
git commit -m "Add the systemd unit, installer and uninstaller

The service runs as an unprivileged remotica user; a sudoers file
permits exactly three systemctl command lines against exactly the
remotica unit, which is what lets the System page toggle boot-start
without the network-facing server ever holding root. The sudoers file
is validated with visudo -c before being moved into place, because a
malformed one can lock the machine out of sudo.

The uninstaller keeps /var/lib/remotica by default -- that's the
printer profile and every uploaded gcode file -- and says so."
```

**Note for the executor:** a real end-to-end install can only be verified on a clean VM, not on this dev machine, where it would install a service and a sudoers file over the top of the user's working setup. Do **not** run `install.sh` locally. Report to the user that VM verification is the remaining gate.

---

## Task 7: Release workflow

**Files:**
- Create: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: the layout Task 6 expects inside the tarball
- Produces: Release assets `remotica-linux-x86_64.tar.gz`, `remotica-linux-aarch64.tar.gz`, `SHA256SUMS`, `install.sh`

- [ ] **Step 1: Write the workflow**

Triggered on `push: tags: ['v*']`. Two jobs.

**Job `build`,** matrix over `x86_64` (`ubuntu-latest`) and `aarch64` (`ubuntu-24.04-arm`), each:
- `actions/checkout@v4` with `fetch-depth: 0` — `git describe` needs tags, and a shallow clone silently yields `dev` as the version.
- `actions/setup-node@v4` with `node-version: 20`, `cache: npm`, `cache-dependency-path: frontend/package-lock.json`
- `cd frontend && npm ci && npm run build`
- `cd backend && make` (the Makefile's `git describe` supplies the version)
- Assemble `remotica-linux-$ARCH/` containing `remotica`, `web/`, `remotica.service`, `99-remotica-serial.rules`, `install.sh`; tar it
- Upload as an artifact

**Job `release`,** `needs: build`:
- Download both artifacts
- `sha256sum *.tar.gz > SHA256SUMS`
- `softprops/action-gh-release@v2` attaching both tarballs, `SHA256SUMS`, and `packaging/install.sh` as a standalone asset (so it can be curled before anything else exists)
- `permissions: contents: write` on the job

- [ ] **Step 2: Validate the YAML**

```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release.yml')); print('yaml ok')"
```

- [ ] **Step 3: Verify the tarball assembles locally**

Run the assemble step's commands by hand and confirm the resulting tree matches what `install.sh` expects — specifically that `web/index.html` and `remotica` are at the paths the installer copies from. A mismatch here only shows up as a broken Release otherwise.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "Build release tarballs for x86_64 and arm64 on tag push

fetch-depth: 0 is load-bearing -- the Makefile takes the version from
git describe, and a shallow clone would silently ship every build as
\"dev\"."
```

---

## Task 8: Documentation

**Files:**
- Modify: `README.md`, `ROADMAP.md`, `CLAUDE.md`, `frontend/CLAUDE.md`

- [ ] **Step 1: README — add Installation as the primary path**

New `## Installation` section, placed **above** the existing "Running it", containing:

- The one-line install command:
  ```sh
  curl -fsSL https://github.com/Yuval-Peleg/remotica/releases/latest/download/install.sh | sudo sh
  ```
- What it puts on the machine — the layout table from the spec.
- That it runs as an unprivileged `remotica` user, and why the sudoers file exists (three exact commands, so the boot toggle works without the server holding root).
- Checking on it: `systemctl status remotica`, `journalctl -u remotica -f`.
- Updating: re-run the same command; the profile and uploaded files survive.
- Uninstalling: `sudo remotica-uninstall`, and that `/var/lib/remotica` is kept unless `--purge`.
- The no-authentication warning, repeated here.

Retitle `## Running it` → `## Running from source (development)` and add a first line making clear it is the dev path, not how to deploy.

- [ ] **Step 2: ROADMAP — remove the resolved item**

Delete the `## Single-process deployment` section entirely. Do **not** touch the hardware-verification sections — none of that changed.

- [ ] **Step 3: CLAUDE.md**

- "Deployment model": stop saying "Not built yet". Describe what now exists (`--web-root`, `--data-dir`, the SPA fallback and why it's needed) and note `run.sh` remains the two-process dev path deliberately.
- Add `packaging/` and `.github/workflows/` to the repo layout list.
- Add `system_info.h/.c` to the source layout list, including the "every command string is a compile-time constant, nothing from the network is interpolated" invariant — that is the kind of constraint a future edit could break silently.
- Add `/api/system` and `/api/system/boot-start` to the `api_handlers.c` route list.
- Under "Not done yet", replace the static-serving bullet with the real remaining gap: the installer has not been verified on a clean VM (unless by then it has).

- [ ] **Step 4: frontend/CLAUDE.md**

Update the `src/pages/` bullet: System is no longer a placeholder. Note the `managed: false` behaviour, since a future reader seeing a permanently-disabled switch in dev would otherwise think it was broken.

- [ ] **Step 5: Verify the docs against reality**

Re-read each claim and check it against the code as built. Specifically confirm the flag names, the installed paths, and the uninstall command all match Tasks 1-7 exactly. Docs that drift from the installer are worse than no docs.

- [ ] **Step 6: Commit**

```bash
cd frontend && npm run format   # picks up the CLAUDE.md files
git add README.md ROADMAP.md CLAUDE.md frontend/CLAUDE.md
git commit -m "Document installing Remotica on the printer PC

Installation becomes the primary path in the README; running from
source is retitled as the development path it actually is. Drops the
single-process roadmap item, which this work resolves."
```

---

## Final verification (whole plan)

Run after all tasks:

```bash
cd backend && make && cppcheck src/ 2>&1 | tail -20
cd ../frontend && npm run lint && npm run build
cd .. && ./run.sh --sim     # dev path still works end to end; Ctrl+C
```

Then, on a **clean Ubuntu VM** — this cannot be done on the dev machine without installing a service and a sudoers file over the user's working setup:

1. Install; answer yes to boot-start.
2. Reboot. Confirm the dashboard is reachable from another device with nobody having logged in.
3. Toggle boot-start off from the System page; confirm with `systemctl is-enabled remotica` on the box. This is the only real test of the sudoers rules.
4. Upload a G-code file, re-run the installer, confirm the file and profile survive.
5. `sudo remotica-uninstall`; confirm every path in the layout table is gone, `/var/lib/remotica` is not, `sudo` still works, and `systemctl status remotica` reports no unit.
6. `sudo remotica-uninstall --purge` removes the data directory too.
