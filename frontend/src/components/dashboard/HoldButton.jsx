import { useCallback, useEffect, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

// How long the press has to be held before it counts.
//
// Chosen against two competing needs: short enough that stopping a print
// in a hurry doesn't feel like fighting the UI, long enough that it can't
// be produced by accident. A confirmation dialog would be safer still,
// but it's *slower* in the emergency this exists for — it costs a second
// tap on a target that appears somewhere new, which is the worst thing to
// ask of someone reaching for the stop button.
const HOLD_MS = 600;

// A press that wanders more than this many pixels is a scroll, not a
// press, and is abandoned. This is the part that actually solves the
// reported problem: on a phone, brushing one of these buttons while
// flicking the page past it can never complete, because the finger is
// moving. Deliberately small — a real press-and-hold barely moves.
const MOVE_CANCEL_PX = 10;

// A button that only fires after being held down, for actions that are
// disruptive to trigger by accident but must stay fast on purpose —
// pausing and stopping a running print.
//
// Nothing here calls preventDefault: the page must still scroll normally
// when a drag starts on top of the button, which is exactly the gesture
// being protected against.
export function HoldButton({
  onConfirm,
  children,
  holdLabel,
  className,
  ...props
}) {
  const [progress, setProgress] = useState(0);
  const [holding, setHolding] = useState(false);
  const frameRef = useRef(null);
  const startRef = useRef({ time: 0, x: 0, y: 0 });
  const firedRef = useRef(false);

  const stop = useCallback(() => {
    cancelAnimationFrame(frameRef.current);
    frameRef.current = null;
    setHolding(false);
    setProgress(0);
  }, []);

  // Progress is driven by rAF against real elapsed time rather than a CSS
  // transition, so what's on screen always matches how much of the hold
  // has actually happened. A CSS transition would be overridden by the
  // app-wide motion kill switch (and by prefers-reduced-motion), which
  // would fill the bar instantly while the action still needed the full
  // hold — showing "done" for something that hadn't happened.
  const begin = useCallback(
    (x, y) => {
      if (frameRef.current !== null) return;
      firedRef.current = false;
      startRef.current = { time: performance.now(), x, y };
      setHolding(true);

      const tick = () => {
        const elapsed = performance.now() - startRef.current.time;
        const ratio = Math.min(1, elapsed / HOLD_MS);
        setProgress(ratio);

        if (ratio < 1) {
          frameRef.current = requestAnimationFrame(tick);
          return;
        }

        if (!firedRef.current) {
          firedRef.current = true;
          onConfirm?.();
        }
        stop();
      };

      frameRef.current = requestAnimationFrame(tick);
    },
    [onConfirm, stop]
  );

  useEffect(() => () => cancelAnimationFrame(frameRef.current), []);

  const handlePointerMove = (e) => {
    if (frameRef.current === null) return;
    const { x, y } = startRef.current;
    if (Math.hypot(e.clientX - x, e.clientY - y) > MOVE_CANCEL_PX) {
      stop();
    }
  };

  return (
    <Button
      {...props}
      className={cn("relative overflow-hidden", className)}
      onPointerDown={(e) => begin(e.clientX, e.clientY)}
      onPointerMove={handlePointerMove}
      onPointerUp={stop}
      onPointerLeave={stop}
      onPointerCancel={stop}
      // Space and Enter would otherwise activate the button on a single
      // press, quietly bypassing the hold for keyboard users. `repeat`
      // guards against the key-repeat stream restarting it.
      onKeyDown={(e) => {
        if ((e.key === " " || e.key === "Enter") && !e.repeat) {
          e.preventDefault();
          begin(0, 0);
        }
      }}
      onKeyUp={stop}
      onClick={(e) => e.preventDefault()}
    >
      <span
        aria-hidden
        className="absolute inset-y-0 left-0 bg-foreground/20"
        style={{ width: `${progress * 100}%` }}
      />
      <span className="relative flex items-center gap-2">
        {holding && holdLabel ? holdLabel : children}
      </span>
    </Button>
  );
}
