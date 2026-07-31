import { useRef, useState } from "react";
import { TEMP_COLORS } from "@/lib/temp-colors";

const WIDTH = 600;
const HEIGHT = 220;
const PAD = { top: 16, right: 14, bottom: 8, left: 30 };
const INNER_WIDTH = WIDTH - PAD.left - PAD.right;
const INNER_HEIGHT = HEIGHT - PAD.top - PAD.bottom;
const GRID_CELL = 20;

function niceMax(value) {
  return Math.max(100, Math.ceil(value / 50) * 50);
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
  const yMax = niceMax(
    Math.max(
      hotendTarget,
      bedTarget,
      ...history.map((p) => p.hotend),
      ...history.map((p) => p.bed)
    )
  );

  const xAt = (i) =>
    PAD.left + (n <= 1 ? INNER_WIDTH : (i / (n - 1)) * INNER_WIDTH);
  const yAt = (v) => PAD.top + INNER_HEIGHT - (v / yMax) * INNER_HEIGHT;

  const hotendPoints = history
    .map((p, i) => `${xAt(i)},${yAt(p.hotend)}`)
    .join(" ");
  const bedPoints = history.map((p, i) => `${xAt(i)},${yAt(p.bed)}`).join(" ");
  const gridValues = [0, yMax / 2, yMax];

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

          {gridValues.map((v) => (
            <g key={v}>
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
                {Math.round(v)}&deg;
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
