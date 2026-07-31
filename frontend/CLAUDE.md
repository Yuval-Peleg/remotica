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
- `src/components/control/` — the printer control panel ("printer blueprint" design: top-down bed schematic for XY jog, vertical drag-rails for Z/E, temp dials, live temperature graph). XY/Z/E jog is fixed at 1mm — no step-size selector, that was deliberately removed.
- `src/components/dashboard/` — left-panel dashboard pieces (connection status badge, gcode dropzone/upload)
- `src/hooks/` — `use-printer-temps.js` (mock live temp/position data), `use-gcode-file.js` (gcode file selection, thumbnail + print-time extraction)
- `src/lib/` — `printer-profile.js` (hardcoded bed size/max Z/etc, will move to backend config later), `gcode-thumbnail.js` + `gcode-print-time.js` (parse slicer-embedded metadata straight out of the .gcode file text), `temp-colors.js`, `utils.js` (`cn()` helper)

## Conventions

- No new features/pages/content without asking first — this has been explicit from the user throughout. Visual/tooling improvements to existing content are fine to proceed on directly.
- No code comments unless the _why_ is genuinely non-obvious (a workaround, a hidden constraint) — not what the code does.
- All printer/job state here is mock data until the backend exists — see root `CLAUDE.md` for the backend plan and what needs to flip from mock to real.
