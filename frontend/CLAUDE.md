# Remotica frontend

Vite + React 19 (JavaScript, not TypeScript) + Tailwind CSS v4 + shadcn/ui (Nova/radix-nova preset, Lucide icons) + react-router-dom v7.

## Commands

- `npm run dev` — dev server (needs Node on PATH; installed via nvm on this machine)
- `npm run build` — production build
- `npm run lint` — oxlint
- `npm run format` / `npm run format:check` — Prettier (`.prettierrc.json`; defaults mostly match, double quotes + semicolons + es5 trailing commas)
- Adding shadcn components: `npx shadcn@latest add <name>` (run from `frontend/`, config is `components.json`)

## Design tokens

All in `src/index.css`, custom dark-only theme (no light mode) mapped into a Tailwind v4 `@theme inline` block. Key values:

- `--background: #1b1c1a`, `--foreground: #e7e7e2`, `--card: #242524`
- `--primary: #6b8f63` (sage green), `--primary-foreground: #10130f`
- `--muted-foreground: #9aa896`, `--border: #33352f`
- `--brand-from: #b2c9ad` / `--brand-to: #4b5945` — gradient used for the "REMOTICA" wordmark
- `--radius: 0.65rem`
- Font: "Kumbh Sans" (loaded via `<link>` in `index.html`, not self-hosted)
- Temperature colors live separately in `src/lib/temp-colors.js` — hotend `#d95926` (orange), bed `#3987e5` (blue), validated as a categorical pair via the `dataviz` skill. Shared between `TempDial` and `TemperatureGraph` so they can't drift apart — always import from there, never hardcode.

## Structure

- `src/pages/` — route-level components (`Dashboard.jsx` and `Settings.jsx` are real; `System`/`About` are still placeholders). `Settings.jsx` edits the backend's `PrinterProfile` (bed size, max Z, min extrude temp, max hotend/bed temp) via `GET`/`POST /api/profile`, shows the live connection status plus a best-effort `M115` firmware-info hint from the backend (never trusted as real model identification — see root `CLAUDE.md`), and has a "quick-fill from a known printer" dropdown sourced from `GET /api/printer-database` (a small hand-curated backend table, not auto-detected).
- `src/components/control/` — the printer control panel ("printer blueprint" design: top-down bed schematic for XY jog, vertical drag-rails for Z/E, temp dials, live temperature graph). XY/Z/E jog is fixed at 1mm — no step-size selector, that was deliberately removed. While a print is active (printing/paused), the whole panel dims with a lock overlay — see `ControlPanel.jsx`.
- `src/components/dashboard/` — left-panel dashboard pieces: connection status badge (shows a best-effort printer name too, via `parsePrinterName()` on the backend's firmware hint), gcode dropzone/upload, `OnDeviceFiles.jsx` (collapsible browser for gcode files already stored on the backend, with thumbnail/print-time preview, select-to-print, and delete), and `CameraView.jsx` (right column, live webcam preview — `GET /api/camera` is checked once on mount; if available, renders `<img src="/api/camera/stream">` directly, since browsers display MJPEG multipart streams natively with no client-side video code needed. Falls back to a "no camera detected" placeholder both when the backend reports none and if the `<img>` fails to load after having been available. Fullscreen button toggles the whole camera card via the native Fullscreen API. While streaming, polls `GET /api/camera`'s `frameSeq` counter every 1.5s and shows a "Live"/"No new frames" badge depending on whether it's advanced within the last 4s — an `<img>` showing an MJPEG multipart stream gives no reliable per-frame browser event to hook into, so this instead piggybacks on the frame-sequence counter the backend's capture thread already tracks internally).
- `src/hooks/` — `use-printer-state.js` (live printer state: initial `GET /api/state` fetch + `/api/ws` WebSocket subscription, with auto-reconnect if the connection drops. The `jog`/`home`/`setHotendTarget`/`setBedTarget` it returns all go through `requireConnected()` first, which shows a toast and skips the API call entirely if `state.connected` is false, instead of letting it hit the backend and come back as a 502), `use-gcode-file.js` (picks + uploads a local gcode file, parsing its thumbnail/print-time client-side for instant feedback), `use-file-preview.js` (fetches + caches thumbnail/print-time for any filename already on the backend, used by the "on device" browser).
- `src/lib/` — `api.js` (thin fetch wrapper around every backend REST route), `gcode-thumbnail.js` + `gcode-print-time.js` (parse slicer-embedded metadata straight out of gcode file text — used both for a freshly-picked local file and for previewing files already on the backend), `temp-colors.js`, `utils.js` (`cn()` helper). The printer profile (bed size, max Z, etc.) is no longer hardcoded here — it's fetched from `GET /api/profile`.

## Conventions

- No new features/pages/content without asking first — this has been explicit from the user throughout. Visual/tooling improvements to existing content are fine to proceed on directly.
- No code comments unless the _why_ is genuinely non-obvious (a workaround, a hidden constraint) — not what the code does.
- All printer/job state comes from the real backend now (`usePrinterState`) — see root `CLAUDE.md` for what the backend still doesn't do for real (actual print streaming, serial hardware verification).
- Anything that changes the printer physically (jog, home, temp, print start/pause/stop) sends exactly one API call per discrete user action — never on every intermediate pointer-move of a drag. `BedSchematic`'s drag-to-jog previews locally and commits a single delta on release, same pattern `AxisRail` already used.
- Global toasts (shadcn's `sonner` — `src/components/ui/sonner.jsx`, mounted once in `App.jsx`) are the pattern for "you tried to do something physical but it can't happen right now" feedback — e.g. `use-printer-state.js`'s `requireConnected()` and `Dashboard.jsx`'s print-start button both toast "Printer is disconnected" rather than letting the action silently no-op. `theme` is hardcoded to `"dark"` on the `Toaster`, not read from `next-themes` — this app has no light mode to switch between (see "Design tokens" above), so that dependency was deliberately left out of `package.json` even though `npx shadcn add sonner` normally pulls it in.
