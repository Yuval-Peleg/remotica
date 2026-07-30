import { useEffect, useRef, useState } from "react";
import { ArrowDown, ArrowLeft, ArrowRight, ArrowUp } from "lucide-react";
import { PRINTER_PROFILE } from "@/lib/printer-profile";

// XY jog is always 1mm resolution — no user-facing step control for this axis.
const SNAP_MM = 1;
const GRID_CELL_MM = 10;
const DOUBLE_TAP_MS = 350;
const DOUBLE_TAP_DIST_PX = 24;

function snap(value, step) {
  return Math.round(value / step) * step;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

export function BedSchematic({ position, onChange, onHome }) {
  const areaRef = useRef(null);
  const [dragging, setDragging] = useState(false);
  const [schematicHeight, setSchematicHeight] = useState(0);
  const lastTapRef = useRef({ time: 0, x: 0, y: 0 });

  // Measured directly (rather than relying on CSS grid/flex stretch, which
  // doesn't reliably match an aspect-ratio sibling's height) so the vertical
  // dimension bar can span the schematic's actual rendered height.
  useEffect(() => {
    const el = areaRef.current;
    if (!el) return;
    const observer = new ResizeObserver((entries) => {
      setSchematicHeight(entries[0].contentRect.height);
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, []);

  const updateFromPointer = (clientX, clientY) => {
    const rect = areaRef.current.getBoundingClientRect();
    const relX = clamp((clientX - rect.left) / rect.width, 0, 1);
    // Bed origin is bottom-left, so invert Y: top of the box = back of the bed.
    const relY = 1 - clamp((clientY - rect.top) / rect.height, 0, 1);
    const x = clamp(
      snap(relX * PRINTER_PROFILE.bedWidthMm, SNAP_MM),
      0,
      PRINTER_PROFILE.bedWidthMm
    );
    const y = clamp(
      snap(relY * PRINTER_PROFILE.bedDepthMm, SNAP_MM),
      0,
      PRINTER_PROFILE.bedDepthMm
    );
    onChange({ x, y });
  };

  const handlePointerDown = (e) => {
    const now = Date.now();
    const last = lastTapRef.current;
    const isDoubleTap =
      now - last.time < DOUBLE_TAP_MS &&
      Math.hypot(e.clientX - last.x, e.clientY - last.y) < DOUBLE_TAP_DIST_PX;

    if (isDoubleTap) {
      lastTapRef.current = { time: 0, x: 0, y: 0 };
      onHome?.();
      return;
    }

    lastTapRef.current = { time: now, x: e.clientX, y: e.clientY };
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(true);
    updateFromPointer(e.clientX, e.clientY);
  };

  const handlePointerMove = (e) => {
    if (!dragging) return;
    updateFromPointer(e.clientX, e.clientY);
  };

  const markerLeftPct = (position.x / PRINTER_PROFILE.bedWidthMm) * 100;
  const markerTopPct = 100 - (position.y / PRINTER_PROFILE.bedDepthMm) * 100;
  const gridDivisions = Math.max(
    2,
    Math.round(PRINTER_PROFILE.bedWidthMm / GRID_CELL_MM)
  );

  return (
    <div className="flex flex-col gap-2">
      <div className="flex h-8 items-center gap-2">
        <div className="w-[1.1rem] shrink-0" />
        <div className="flex flex-1 items-center text-muted-foreground">
          <ArrowLeft className="size-3.5 shrink-0" />
          <div className="mx-1 h-px flex-1 bg-muted-foreground/50" />
          <span className="shrink-0 bg-card px-1 text-[10px]">
            {PRINTER_PROFILE.bedWidthMm}mm
          </span>
          <div className="mx-1 h-px flex-1 bg-muted-foreground/50" />
          <ArrowRight className="size-3.5 shrink-0" />
        </div>
      </div>

      <div className="flex gap-2">
        <div
          className="flex w-[1.1rem] shrink-0 flex-col items-center text-muted-foreground"
          style={{ height: schematicHeight || undefined }}
        >
          <ArrowUp className="size-3.5 shrink-0" />
          <div className="my-1 w-px flex-1 bg-muted-foreground/50" />
          <span className="shrink-0 bg-card px-0.5 text-[10px] [writing-mode:vertical-rl] rotate-180">
            {PRINTER_PROFILE.bedDepthMm}mm
          </span>
          <div className="my-1 w-px flex-1 bg-muted-foreground/50" />
          <ArrowDown className="size-3.5 shrink-0" />
        </div>

        <div
          ref={areaRef}
          onPointerDown={handlePointerDown}
          onPointerMove={handlePointerMove}
          onPointerUp={() => setDragging(false)}
          onPointerCancel={() => setDragging(false)}
          className="relative aspect-square w-full flex-1 touch-none cursor-crosshair rounded-lg border border-border bg-secondary/40 bg-[linear-gradient(to_right,var(--color-border)_1px,transparent_1px),linear-gradient(to_bottom,var(--color-border)_1px,transparent_1px)]"
          style={{
            backgroundSize: `${100 / gridDivisions}% ${100 / gridDivisions}%`,
          }}
        >
          <div
            className="absolute size-3.5 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 border-primary bg-primary/40"
            style={{ left: `${markerLeftPct}%`, top: `${markerTopPct}%` }}
          />
        </div>
      </div>

      <div className="flex flex-col items-center text-center text-xs text-muted-foreground">
        <p>
          X {position.x.toFixed(1)}mm &middot; Y {position.y.toFixed(1)}mm
        </p>
        <p>tap or drag to jog &middot; double-tap to home</p>
      </div>
    </div>
  );
}
