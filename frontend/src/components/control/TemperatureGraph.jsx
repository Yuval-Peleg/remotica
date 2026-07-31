import { useRef, useState } from "react";
import { TEMP_COLORS } from "@/lib/temp-colors";

const WIDTH = 600;
const HEIGHT = 220;
const PAD = { top: 16, right: 14, bottom: 20, left: 30 };
const INNER_WIDTH = WIDTH - PAD.left - PAD.right;
const INNER_HEIGHT = HEIGHT - PAD.top - PAD.bottom;
const GRID_CELL = 20;

// How many gridlines each axis aims to show. This is a target, not a
// guarantee — niceStep() below picks the nearest "round" step (1, 2, 5,
// or 10 times a power of ten) to that target, the same approach charting
// libraries like D3 use, so lines land on numbers like 5, 10, 25, 50
// instead of awkward ones like 33.3.
const Y_AXIS_TARGET_TICKS = 6;
const X_AXIS_TARGET_TICKS = 6;

function niceStep(range, targetCount) {
  const roughStep = range / targetCount;
  const magnitude = Math.pow(10, Math.floor(Math.log10(roughStep)));
  const normalized = roughStep / magnitude; // now somewhere in [1, 10)
  let niceNormalized;
  if (normalized < 1.5) niceNormalized = 1;
  else if (normalized < 3) niceNormalized = 2;
  else if (normalized < 7) niceNormalized = 5;
  else niceNormalized = 10;
  return niceNormalized * magnitude;
}

// Picks the temperature axis's top value and gridline spacing. A small
// floor (20, padded to ~23) keeps gridlines at a readable ~5deg apart
// while everything's near ambient temperature, instead of showing one
// giant empty 0-300 axis; the same "nice step" logic then scales that
// spacing up gracefully as temperatures actually climb, rather than
// cramming in dozens of 5deg lines once a hotend target is 200+.
function computeYAxis(maxValueInView) {
  const paddedMax = Math.max(20, maxValueInView * 1.15);
  const step = niceStep(paddedMax, Y_AXIS_TARGET_TICKS);
  const axisMax = Math.ceil(paddedMax / step) * step;

  const ticks = [];
  for (let v = 0; v <= axisMax + 0.001; v += step) {
    ticks.push(Math.round(v));
  }
  return { axisMax, ticks };
}

// Picks the elapsed-time gridline spacing (in whole seconds — fractional
// seconds would be a strange thing to label). Unlike the Y axis, this
// doesn't round the visible span itself up to a "nice" number: the graph
// always shows exactly the history it has, ticks are just placed at nice
// points within that fixed span.
function computeXTicks(totalSeconds) {
  const step = Math.max(
    1,
    Math.round(
      niceStep(Math.max(totalSeconds, X_AXIS_TARGET_TICKS), X_AXIS_TARGET_TICKS)
    )
  );

  const ticks = [];
  for (let v = 0; v <= totalSeconds + 0.001; v += step) {
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

export function TemperatureGraph({ history, hotendTarget, bedTarget }) {
  const containerRef = useRef(null);
  const [hoverIndex, setHoverIndex] = useState(null);

  const n = history.length;
  const maxValueInView = Math.max(
    hotendTarget,
    bedTarget,
    ...history.map((p) => p.hotend),
    ...history.map((p) => p.bed)
  );
  const { axisMax: yMax, ticks: yTicks } = computeYAxis(maxValueInView);

  const totalSeconds = n > 1 ? (history[n - 1].t - history[0].t) / 1000 : 0;
  const xTicks = computeXTicks(totalSeconds);

  const xAt = (i) =>
    PAD.left + (n <= 1 ? INNER_WIDTH : (i / (n - 1)) * INNER_WIDTH);
  const yAt = (v) => PAD.top + INNER_HEIGHT - (v / yMax) * INNER_HEIGHT;
  // Elapsed seconds are placed proportionally along the same axis the
  // data uses, which is accurate as long as samples arrive at a roughly
  // steady rate (they do — see the backend's ~300ms tick) — this keeps
  // tick labels aligned with the plotted line without needing every
  // point's exact timestamp for positioning.
  const xAtSeconds = (seconds) =>
    PAD.left + (totalSeconds <= 0 ? 0 : (seconds / totalSeconds) * INNER_WIDTH);

  const hotendPoints = history
    .map((p, i) => `${xAt(i)},${yAt(p.hotend)}`)
    .join(" ");
  const bedPoints = history.map((p, i) => `${xAt(i)},${yAt(p.bed)}`).join(" ");

  const lastHotend = history[n - 1]?.hotend ?? 0;
  const lastBed = history[n - 1]?.bed ?? 0;

  const handlePointerMove = (e) => {
    if (!containerRef.current || n === 0) return;
    const rect = containerRef.current.getBoundingClientRect();
    const relX = (e.clientX - rect.left) / rect.width;
    const vbX = relX * WIDTH;
    const t = (vbX - PAD.left) / INNER_WIDTH;
    const idx = Math.round(t * (n - 1));
    setHoverIndex(Math.min(Math.max(idx, 0), n - 1));
  };

  const hovered = hoverIndex !== null ? history[hoverIndex] : null;

  return (
    <div className="flex flex-col gap-3">
      <div className="flex items-center gap-4 text-xs text-muted-foreground">
        <LegendKey color={TEMP_COLORS.hotend} label="Hotend" />
        <LegendKey color={TEMP_COLORS.bed} label="Bed" />
      </div>

      <div
        ref={containerRef}
        className="relative w-full touch-none"
        style={{ aspectRatio: `${WIDTH} / ${HEIGHT}` }}
        onPointerMove={handlePointerMove}
        onPointerLeave={() => setHoverIndex(null)}
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

          {hovered && (
            <>
              <line
                x1={xAt(hoverIndex)}
                x2={xAt(hoverIndex)}
                y1={PAD.top}
                y2={HEIGHT - PAD.bottom}
                strokeWidth={1}
                className="stroke-border"
              />
              <circle
                cx={xAt(hoverIndex)}
                cy={yAt(hovered.bed)}
                r={4}
                fill={TEMP_COLORS.bed}
                className="stroke-card"
                strokeWidth={2}
              />
              <circle
                cx={xAt(hoverIndex)}
                cy={yAt(hovered.hotend)}
                r={4}
                fill={TEMP_COLORS.hotend}
                className="stroke-card"
                strokeWidth={2}
              />
            </>
          )}
        </svg>

        {hovered && (
          <div
            className="pointer-events-none absolute top-2 flex -translate-x-1/2 flex-col gap-1 rounded-md border border-border bg-popover px-2.5 py-2 text-xs whitespace-nowrap shadow-md"
            style={{ left: `${(xAt(hoverIndex) / WIDTH) * 100}%` }}
          >
            <span className="text-[10px] text-muted-foreground">
              {new Date(hovered.t).toLocaleTimeString([], {
                minute: "2-digit",
                second: "2-digit",
              })}
            </span>
            <span className="flex items-center gap-1.5">
              <span
                className="inline-block h-0.5 w-3 rounded-full"
                style={{ backgroundColor: TEMP_COLORS.hotend }}
              />
              <span className="font-medium text-foreground">
                {Math.round(hovered.hotend)}&deg;C
              </span>
              <span className="text-muted-foreground">Hotend</span>
            </span>
            <span className="flex items-center gap-1.5">
              <span
                className="inline-block h-0.5 w-3 rounded-full"
                style={{ backgroundColor: TEMP_COLORS.bed }}
              />
              <span className="font-medium text-foreground">
                {Math.round(hovered.bed)}&deg;C
              </span>
              <span className="text-muted-foreground">Bed</span>
            </span>
          </div>
        )}
      </div>
    </div>
  );
}
