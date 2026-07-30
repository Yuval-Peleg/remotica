import { useRef, useState } from "react";
import { cn } from "@/lib/utils";

const RAIL_HEIGHT = 160;
const MAX_NOTCHES = 10;
const NOTCH_PX = RAIL_HEIGHT / (MAX_NOTCHES * 2);
const STEP_MM = 1;

export function AxisRail({
  label,
  unit = "mm",
  valueLabel,
  onJog,
  disabled = false,
  disabledReason,
}) {
  const dragStartY = useRef(0);
  const [dragOffset, setDragOffset] = useState(0);
  const [dragging, setDragging] = useState(false);

  const handlePointerDown = (e) => {
    if (disabled) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    dragStartY.current = e.clientY;
    setDragging(true);
  };

  const handlePointerMove = (e) => {
    if (!dragging) return;
    const dy = e.clientY - dragStartY.current;
    setDragOffset(Math.min(Math.max(dy, -RAIL_HEIGHT / 2), RAIL_HEIGHT / 2));
  };

  const commit = () => {
    if (!dragging) return;
    setDragging(false);
    const notches = Math.round(dragOffset / NOTCH_PX);
    setDragOffset(0);
    if (notches !== 0) onJog(-notches * STEP_MM);
  };

  const previewMm = -Math.round(dragOffset / NOTCH_PX) * STEP_MM;

  return (
    <div className="flex w-20 shrink-0 flex-col items-center gap-1.5">
      <span className="text-xs font-medium text-muted-foreground">
        {label}
      </span>
      <div
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={commit}
        onPointerCancel={commit}
        className={cn(
          "relative w-8 touch-none select-none rounded-full bg-muted",
          disabled ? "cursor-not-allowed opacity-40" : "cursor-ns-resize"
        )}
        style={{ height: RAIL_HEIGHT }}
        title={disabled ? disabledReason : undefined}
      >
        <div className="absolute inset-x-0 top-1/2 h-px bg-border" />
        <div
          className="absolute left-1/2 size-6 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 border-primary bg-card shadow"
          style={{ top: `calc(50% + ${dragOffset}px)` }}
        />
      </div>
      <span className="text-xs tabular-nums text-foreground">
        {dragging
          ? `${previewMm > 0 ? "+" : ""}${previewMm.toFixed(1)}${unit}`
          : valueLabel}
      </span>
    </div>
  );
}
