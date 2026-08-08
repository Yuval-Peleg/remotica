import { useEffect, useState } from "react";
import { Minus, Plus } from "lucide-react";
import { Button } from "@/components/ui/button";

const MAX_TEMP = 300;
const SIZE = 88;
const STROKE_WIDTH = 8;
const RADIUS = (SIZE - STROKE_WIDTH) / 2;

// The ring's outer edge lands at exactly RADIUS + STROKE_WIDTH / 2, which
// is SIZE / 2 — flush against the SVG's own edge. An SVG clips to its
// viewport, so the heating glow below (a drop-shadow that spreads several
// pixels outward) was painted outside it and sliced off square. This pads
// the canvas so the glow has somewhere to go. The SVG is positioned by
// this same inset, so the dial's layout size is unchanged — the padding
// is transparent overspill, not extra space in the row.
const GLOW_PADDING = 10;
const CANVAS = SIZE + GLOW_PADDING * 2;
const CIRCUMFERENCE = 2 * Math.PI * RADIUS;
// A real reading hovering right around 0 (ambient temp with sensor jitter,
// see transport_sim.c) would otherwise push the ring's progress to exactly
// 0 sometimes and a hair above it other times — the colored arc would
// fully vanish then instantly reappear as a sliver, which reads as
// flickering rather than "it's just near zero". Flooring it at a small
// visible minimum keeps a constant sliver on screen so the ring never
// disappears, regardless of how close to 0 the actual reading is.
const MIN_VISIBLE_PROGRESS = 0.015;

export function TempDial({
  label,
  icon: Icon,
  color,
  current,
  target,
  onTargetChange,
  step = 5,
  minTarget = 0,
  maxTarget = 280,
}) {
  const progress = Math.max(
    Math.min(current / MAX_TEMP, 1),
    MIN_VISIBLE_PROGRESS
  );
  // 2°C of slack so ordinary sensor jitter at a reached target doesn't
  // flicker the glow on and off.
  const isHeating = target > current + 2;
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(String(target));

  useEffect(() => {
    if (!editing) setDraft(String(target));
  }, [target, editing]);

  const commitDraft = () => {
    const parsed = Number(draft);
    if (Number.isFinite(parsed)) {
      onTargetChange(Math.min(maxTarget, Math.max(minTarget, parsed)));
    }
    setEditing(false);
  };

  return (
    <div className="flex flex-1 flex-col items-center gap-2 rounded-lg border border-border bg-card p-4">
      <div className="flex items-center gap-1.5 text-xs font-medium text-muted-foreground">
        {Icon && <Icon className="size-3.5" />}
        {label}
      </div>

      <div className="relative" style={{ width: SIZE, height: SIZE }}>
        <svg
          width={CANVAS}
          height={CANVAS}
          className="absolute -rotate-90 overflow-visible"
          style={{ left: -GLOW_PADDING, top: -GLOW_PADDING }}
        >
          <circle
            cx={CANVAS / 2}
            cy={CANVAS / 2}
            r={RADIUS}
            strokeWidth={STROKE_WIDTH}
            className="fill-none stroke-muted"
          />
          <circle
            cx={CANVAS / 2}
            cy={CANVAS / 2}
            r={RADIUS}
            strokeWidth={STROKE_WIDTH}
            strokeLinecap="round"
            stroke={color}
            className="fill-none transition-[stroke-dashoffset,filter] duration-700 ease-soft"
            strokeDasharray={CIRCUMFERENCE}
            strokeDashoffset={CIRCUMFERENCE * (1 - progress)}
            // Glows while the heater is actually climbing towards a
            // target. Static rather than pulsing on purpose — a readout
            // that throbs is harder to read at a glance, and this one is
            // next to a number someone may be checking against a
            // filament spec.
            style={
              isHeating
                ? { filter: `drop-shadow(0 0 4px ${color})` }
                : undefined
            }
          />
        </svg>
        <div className="absolute inset-0 flex flex-col items-center justify-center">
          <span className="text-sm font-semibold tabular-nums text-foreground">
            {Math.round(current)}&deg;
          </span>
          <span className="text-[10px] text-muted-foreground">
            / {target}&deg;C
          </span>
        </div>
      </div>

      <div className="flex items-center gap-1">
        <Button
          variant="outline"
          size="icon-sm"
          onClick={() => onTargetChange(Math.max(minTarget, target - step))}
        >
          <Minus />
        </Button>
        {editing ? (
          <input
            type="number"
            autoFocus
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            onFocus={(e) => e.target.select()}
            onBlur={commitDraft}
            onKeyDown={(e) => {
              if (e.key === "Enter") commitDraft();
              if (e.key === "Escape") setEditing(false);
            }}
            className="w-14 rounded-md border border-input bg-transparent px-1.5 py-1 text-center text-xs tabular-nums text-foreground outline-none transition-colors [appearance:textfield] focus-visible:border-ring focus-visible:ring-3 focus-visible:ring-ring/50 [&::-webkit-inner-spin-button]:appearance-none [&::-webkit-outer-spin-button]:appearance-none"
          />
        ) : (
          <button
            type="button"
            onClick={() => setEditing(true)}
            className="w-12 text-center text-xs tabular-nums text-foreground hover:underline"
          >
            {target}&deg;C
          </button>
        )}
        <Button
          variant="outline"
          size="icon-sm"
          onClick={() => onTargetChange(Math.min(maxTarget, target + step))}
        >
          <Plus />
        </Button>
      </div>
    </div>
  );
}
