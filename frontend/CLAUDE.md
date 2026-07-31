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

- `src/pages/` — route-level components (`Dashboard.jsx` is the real one; `System`/`Settings`/`About` are placeholders)
- `src/components/control/` — the printer control panel ("printer blueprint" design: top-down bed schematic for XY jog, vertical drag-rails for Z/E, temp dials, live temperature graph). XY/Z/E jog is fixed at 1mm — no step-size selector, that was deliberately removed. While a print is active (printing/paused), the whole panel dims with a lock overlay — see `ControlPanel.jsx`.
- `src/components/dashboard/` — left-panel dashboard pieces: connection status badge, gcode dropzone/upload, and `OnDeviceFiles.jsx` (collapsible browser for gcode files already stored on the backend, with thumbnail/print-time preview, select-to-print, and delete).
- `src/hooks/` — `use-printer-state.js` (live printer state: initial `GET /api/state` fetch + `/api/ws` WebSocket subscription, with auto-reconnect if the connection drops), `use-gcode-file.js` (picks + uploads a local gcode file, parsing its thumbnail/print-time client-side for instant feedback), `use-file-preview.js` (fetches + caches thumbnail/print-time for any filename already on the backend, used by the "on device" browser).
- `src/lib/` — `api.js` (thin fetch wrapper around every backend REST route), `gcode-thumbnail.js` + `gcode-print-time.js` (parse slicer-embedded metadata straight out of gcode file text — used both for a freshly-picked local file and for previewing files already on the backend), `temp-colors.js`, `utils.js` (`cn()` helper). The printer profile (bed size, max Z, etc.) is no longer hardcoded here — it's fetched from `GET /api/profile`.

## Conventions

- No new features/pages/content without asking first — this has been explicit from the user throughout. Visual/tooling improvements to existing content are fine to proceed on directly.
- No code comments unless the _why_ is genuinely non-obvious (a workaround, a hidden constraint) — not what the code does.
- All printer/job state comes from the real backend now (`usePrinterState`) — see root `CLAUDE.md` for what the backend still doesn't do for real (actual print streaming, serial hardware verification).
- Anything that changes the printer physically (jog, home, temp, print start/pause/stop) sends exactly one API call per discrete user action — never on every intermediate pointer-move of a drag. `BedSchematic`'s drag-to-jog previews locally and commits a single delta on release, same pattern `AxisRail` already used.
