import { useEffect, useRef, useState } from "react";
import { ArrowDown, ArrowLeft, ArrowRight, ArrowUp, Home } from "lucide-react";
import { cn } from "@/lib/utils";

// XY jog is always 1mm resolution — no user-facing step control for this axis.
const SNAP_MM = 1;
const GRID_CELL_MM = 10;
const DOUBLE_TAP_MS = 350;
const DOUBLE_TAP_DIST_PX = 24;
// Safety net for the drag preview (see the effect below): if the real
// position never quite matches what we asked for — the jog request
// failed, or landed on a slightly different clamped value — don't leave
// the marker stuck showing a stale preview forever.
const PREVIEW_FALLBACK_MS = 1000;
const POSITION_EPSILON_MM = 0.001;

function snap(value, step) {
  return Math.round(value / step) * step;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

// `position` is the authoritative position from the backend. `onJog(axis,
// deltaMm)` is called at most twice per drag gesture (once for X, once for
// Y) — right when the pointer is released, not on every pointermove — so
// dragging around the bed doesn't fire dozens of network requests. While
// actively dragging (and for a moment after release, until the backend's
// real position catches up), the marker/coordinates show a locally-tracked
// preview position instead of `position`, so the drag feels instant and
// doesn't visibly snap back to the old spot while waiting on the network.
//
// `homing`: true while a home command is in flight (see ControlPanel,
// which owns this state around its onHome call). A real G28 blocks the
// backend's serial driver for as long as the printer takes to physically
// home — during that window neither position nor temperature actually
// update (the driver has the port lock held the whole time, and there's
// no way to poll position from real hardware at all — see transport_
// serial.c), so showing the stale, unmoving marker/coordinates as if
// they were live would be misleading. Grays the whole schematic out with
// a "Homing..." indicator instead, and blocks further taps/drags until
// it's done.
export function BedSchematic({
  position,
  bedWidthMm,
  bedDepthMm,
  onJog,
  onHome,
  homing = false,
}) {
  const areaRef = useRef(null);
  const [dragging, setDragging] = useState(false);
  const [previewPosition, setPreviewPosition] = useState(null);
  const [schematicHeight, setSchematicHeight] = useState(0);
  const lastTapRef = useRef({ time: 0, x: 0, y: 0 });
  const dragStartPositionRef = useRef(position);
  const previewFallbackTimerRef = useRef(null);

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

  // Once the backend reports back a position that matches what we last
  // committed, drop the local preview and let the real position take over
  // — clearing it immediately on release would flash back to the pre-drag
  // spot for the ~300ms until the next state update arrives, which looked
  // like the marker jumping backward before jumping to the right place.
  useEffect(() => {
    if (!previewPosition) return;
    const matches =
      Math.abs(position.x - previewPosition.x) < POSITION_EPSILON_MM &&
      Math.abs(position.y - previewPosition.y) < POSITION_EPSILON_MM;
    if (matches) {
      clearTimeout(previewFallbackTimerRef.current);
      setPreviewPosition(null);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [position]);

  useEffect(() => () => clearTimeout(previewFallbackTimerRef.current), []);

  const updateFromPointer = (clientX, clientY) => {
    const rect = areaRef.current.getBoundingClientRect();
    const relX = clamp((clientX - rect.left) / rect.width, 0, 1);
    // Bed origin is bottom-left, so invert Y: top of the box = back of the bed.
    const relY = 1 - clamp((clientY - rect.top) / rect.height, 0, 1);
    const x = clamp(snap(relX * bedWidthMm, SNAP_MM), 0, bedWidthMm);
    const y = clamp(snap(relY * bedDepthMm, SNAP_MM), 0, bedDepthMm);
    setPreviewPosition({ x, y });
  };

  const handlePointerDown = (e) => {
    const now = Date.now();
    const last = lastTapRef.current;
    const isDoubleTap =
      now - last.time < DOUBLE_TAP_MS &&
      Math.hypot(e.clientX - last.x, e.clientY - last.y) < DOUBLE_TAP_DIST_PX;

    if (isDoubleTap) {
      lastTapRef.current = { time: 0, x: 0, y: 0 };
      if (!homing) onHome?.();
      return;
    }

    lastTapRef.current = { time: now, x: e.clientX, y: e.clientY };
    dragStartPositionRef.current = position;
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(true);
    updateFromPointer(e.clientX, e.clientY);
  };

  const handlePointerMove = (e) => {
    if (!dragging) return;
    updateFromPointer(e.clientX, e.clientY);
  };

  const commit = () => {
    if (!dragging) return;
    setDragging(false);

    const target = previewPosition;
    if (!target) return;

    const deltaX = target.x - dragStartPositionRef.current.x;
    const deltaY = target.y - dragStartPositionRef.current.y;

    if (deltaX === 0 && deltaY === 0) {
      setPreviewPosition(null);
      return;
    }

    if (deltaX !== 0) onJog("X", deltaX);
    if (deltaY !== 0) onJog("Y", deltaY);

    // Keep showing `target` (set above, already the current previewPosition)
    // until the effect above sees the real position catch up, with a
    // timeout fallback in case it never quite matches (a failed request,
    // an unexpected clamp) so the marker doesn't get stuck.
    clearTimeout(previewFallbackTimerRef.current);
    previewFallbackTimerRef.current = setTimeout(
      () => setPreviewPosition(null),
      PREVIEW_FALLBACK_MS
    );
  };

  const displayPosition = previewPosition ?? position;
  const isHomed = position.x === 0 && position.y === 0;
  const markerLeftPct = (displayPosition.x / bedWidthMm) * 100;
  const markerTopPct = 100 - (displayPosition.y / bedDepthMm) * 100;
  const gridDivisions = Math.max(2, Math.round(bedWidthMm / GRID_CELL_MM));

  return (
    <div className="relative">
      <div
        className={cn(
          "flex select-none flex-col gap-2",
          homing && "pointer-events-none opacity-30 blur-[1px]"
        )}
      >
        <div className="flex h-8 items-center gap-2">
          <div className="w-[1.1rem] shrink-0" />
          <div className="flex flex-1 items-center text-muted-foreground">
            <ArrowLeft className="size-3.5 shrink-0" />
            <div className="mx-1 h-px flex-1 bg-muted-foreground/50" />
            <span className="shrink-0 bg-card px-1 text-[10px]">
              {bedWidthMm}mm
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
              {bedDepthMm}mm
            </span>
            <div className="my-1 w-px flex-1 bg-muted-foreground/50" />
            <ArrowDown className="size-3.5 shrink-0" />
          </div>

          <div
            ref={areaRef}
            onPointerDown={handlePointerDown}
            onPointerMove={handlePointerMove}
            onPointerUp={commit}
            onPointerCancel={commit}
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
          <p className="flex items-center justify-center gap-1.5">
            {isHomed && <Home className="size-3 shrink-0 text-primary" />}
            <span>
              X {displayPosition.x.toFixed(1)}mm &middot; Y{" "}
              {displayPosition.y.toFixed(1)}mm
            </span>
          </p>
          <p>tap or drag to jog &middot; double-tap to home</p>
        </div>
      </div>

      {homing && (
        <div className="absolute inset-0 z-10 flex flex-col items-center justify-center gap-2 rounded-lg bg-background/70 text-center backdrop-blur-sm">
          <Home className="size-8 animate-pulse text-primary" />
          <p className="text-sm font-medium text-muted-foreground">
            Homing&hellip;
          </p>
        </div>
      )}
    </div>
  );
}
