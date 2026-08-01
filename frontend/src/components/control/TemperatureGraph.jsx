import { TEMP_COLORS } from "@/lib/temp-colors";

const WIDTH = 600;
const HEIGHT = 220;
const PAD = { top: 16, right: 14, bottom: 20, left: 30 };
const INNER_WIDTH = WIDTH - PAD.left - PAD.right;
const INNER_HEIGHT = HEIGHT - PAD.top - PAD.bottom;
const GRID_CELL = 20;

// Fixed axes, deliberately not recomputed from the live data: a panel
// that keeps rescaling itself while you're trying to read it is harder to
// read, not easier. The X axis matches use-printer-state.js's
// HISTORY_LENGTH (~60s of ticks at the backend's ~300ms cadence), so the
// full axis is always exactly as wide as the history buffer can get.
const Y_AXIS_TICK_STEP = 50;
const X_AXIS_MAX_SECONDS = 60;
const X_AXIS_TICK_STEP = 10;

function tickRange(max, step) {
  const ticks = [];
  for (let v = 0; v <= max + 0.001; v += step) {
    ticks.push(Math.round(v));
  }
  return ticks;
}

function LegendKey({ color, label }) {
  return (
    <span className="flex items-center gap-1.5">
      <span
        className="inline-block h-0.5 w-4 rounded-full"
        style={{ backgroundColor: color }}
      />
      {label}
    </span>
  );
}

export function TemperatureGraph({ history, profile }) {
  const n = history.length;

  // Axis maxed out at the printer's own physical limits, per profile —
  // fixed, not driven by whatever the current/target temps happen to be.
  const yMax = Math.max(profile.maxHotendTempC, profile.maxBedTempC);
  const yTicks = tickRange(yMax, Y_AXIS_TICK_STEP);
  const xTicks = tickRange(X_AXIS_MAX_SECONDS, X_AXIS_TICK_STEP);

  // Points are placed by real elapsed time since the oldest sample still
  // in the history buffer, against the fixed 60s axis above — not spread
  // evenly across the width by index — so a freshly (re)connected session
  // fills in from the left instead of stretching a handful of points
  // across the full width.
  const startMs = history[0]?.t ?? 0;
  const xAt = (i) =>
    PAD.left +
    Math.min(1, (history[i].t - startMs) / 1000 / X_AXIS_MAX_SECONDS) *
      INNER_WIDTH;
  const yAt = (v) =>
    PAD.top + INNER_HEIGHT - (Math.min(v, yMax) / yMax) * INNER_HEIGHT;
  const xAtSeconds = (seconds) =>
    PAD.left + (seconds / X_AXIS_MAX_SECONDS) * INNER_WIDTH;

  const hotendPoints = history
    .map((p, i) => `${xAt(i)},${yAt(p.hotend)}`)
    .join(" ");
  const bedPoints = history.map((p, i) => `${xAt(i)},${yAt(p.bed)}`).join(" ");

  const lastHotend = history[n - 1]?.hotend ?? 0;
  const lastBed = history[n - 1]?.bed ?? 0;

  return (
    <div className="flex flex-col gap-3">
      <div className="flex items-center gap-4 text-xs text-muted-foreground">
        <LegendKey color={TEMP_COLORS.hotend} label="Hotend" />
        <LegendKey color={TEMP_COLORS.bed} label="Bed" />
      </div>

      <div
        className="relative w-full"
        style={{ aspectRatio: `${WIDTH} / ${HEIGHT}` }}
      >
        <svg viewBox={`0 0 ${WIDTH} ${HEIGHT}`} className="h-full w-full">
          <defs>
            <pattern
              id="temp-grid-cell"
              width={GRID_CELL}
              height={GRID_CELL}
              patternUnits="userSpaceOnUse"
            >
              <path
                d={`M ${GRID_CELL} 0 L 0 0 0 ${GRID_CELL}`}
                fill="none"
                className="stroke-border"
                strokeWidth={1}
              />
            </pattern>
            <clipPath id="temp-plot-clip">
              <rect
                x={PAD.left}
                y={PAD.top}
                width={INNER_WIDTH}
                height={INNER_HEIGHT}
                rx={6}
              />
            </clipPath>
          </defs>

          <g clipPath="url(#temp-plot-clip)">
            <rect
              x={PAD.left}
              y={PAD.top}
              width={INNER_WIDTH}
              height={INNER_HEIGHT}
              className="fill-secondary/40"
            />
            <rect
              x={PAD.left}
              y={PAD.top}
              width={INNER_WIDTH}
              height={INNER_HEIGHT}
              fill="url(#temp-grid-cell)"
            />
          </g>
          <rect
            x={PAD.left}
            y={PAD.top}
            width={INNER_WIDTH}
            height={INNER_HEIGHT}
            rx={6}
            fill="none"
            className="stroke-border"
            strokeWidth={1}
          />

          {yTicks.map((v) => (
            <g key={`y-${v}`}>
              <line
                x1={PAD.left}
                x2={WIDTH - PAD.right}
                y1={yAt(v)}
                y2={yAt(v)}
                strokeWidth={1}
                className="stroke-border"
              />
              <text
                x={PAD.left - 6}
                y={yAt(v)}
                textAnchor="end"
                dominantBaseline="middle"
                className="fill-muted-foreground text-[9px]"
              >
                {v}&deg;
              </text>
            </g>
          ))}

          {xTicks.map((v) => (
            <g key={`x-${v}`}>
              <line
                x1={xAtSeconds(v)}
                x2={xAtSeconds(v)}
                y1={PAD.top}
                y2={HEIGHT - PAD.bottom}
                strokeWidth={1}
                className="stroke-border"
              />
              <text
                x={xAtSeconds(v)}
                y={HEIGHT - PAD.bottom + 12}
                textAnchor="middle"
                className="fill-muted-foreground text-[9px]"
              >
                {v}s
              </text>
            </g>
          ))}

          <polyline
            points={bedPoints}
            fill="none"
            stroke={TEMP_COLORS.bed}
            strokeWidth={2}
            strokeLinejoin="round"
            strokeLinecap="round"
          />
          <polyline
            points={hotendPoints}
            fill="none"
            stroke={TEMP_COLORS.hotend}
            strokeWidth={2}
            strokeLinejoin="round"
            strokeLinecap="round"
          />

          {n > 0 && (
            <>
              <circle
                cx={xAt(n - 1)}
                cy={yAt(lastBed)}
                r={5}
                className="fill-card"
              />
              <circle
                cx={xAt(n - 1)}
                cy={yAt(lastBed)}
                r={3.5}
                fill={TEMP_COLORS.bed}
              />
              <text
                x={xAt(n - 1) - 8}
                y={yAt(lastBed) - 8}
                textAnchor="end"
                className="fill-muted-foreground text-[9px]"
              >
                {Math.round(lastBed)}&deg;
              </text>

              <circle
                cx={xAt(n - 1)}
                cy={yAt(lastHotend)}
                r={5}
                className="fill-card"
              />
              <circle
                cx={xAt(n - 1)}
                cy={yAt(lastHotend)}
                r={3.5}
                fill={TEMP_COLORS.hotend}
              />
              <text
                x={xAt(n - 1) - 8}
                y={yAt(lastHotend) - 8}
                textAnchor="end"
                className="fill-muted-foreground text-[9px]"
              >
                {Math.round(lastHotend)}&deg;
              </text>
            </>
          )}
        </svg>
      </div>
    </div>
  );
}
