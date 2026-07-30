import { useEffect, useState } from "react";
import { Minus, Plus } from "lucide-react";
import { Button } from "@/components/ui/button";

const MAX_TEMP = 300;
const SIZE = 88;
const STROKE_WIDTH = 8;
const RADIUS = (SIZE - STROKE_WIDTH) / 2;
const CIRCUMFERENCE = 2 * Math.PI * RADIUS;

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
  const progress = Math.min(current / MAX_TEMP, 1);
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
        <svg width={SIZE} height={SIZE} className="-rotate-90">
          <circle
            cx={SIZE / 2}
            cy={SIZE / 2}
            r={RADIUS}
            strokeWidth={STROKE_WIDTH}
            className="fill-none stroke-muted"
          />
          <circle
            cx={SIZE / 2}
            cy={SIZE / 2}
            r={RADIUS}
            strokeWidth={STROKE_WIDTH}
            strokeLinecap="round"
            stroke={color}
            className="fill-none transition-[stroke-dashoffset] duration-500"
            strokeDasharray={CIRCUMFERENCE}
            strokeDashoffset={CIRCUMFERENCE * (1 - progress)}
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
            className="w-12 rounded border border-ring bg-background text-center text-xs tabular-nums text-foreground outline-none"
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
