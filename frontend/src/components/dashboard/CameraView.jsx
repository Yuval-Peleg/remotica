import { useEffect, useRef, useState } from "react";
import { CameraOff, Maximize, Minimize } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { api } from "@/lib/api";
import { cn } from "@/lib/utils";

// How often to poll GET /api/camera for its frameSeq counter while a
// stream is open, and how long that counter can go unchanged before the
// picture is treated as stalled rather than live — see the liveStatus
// effect below for why this exists at all (an MJPEG <img> tag gives no
// reliable per-frame event to hook into, but the backend's capture
// thread already tracks a sequence number for its own "keep only the
// newest frame" bookkeeping, so this just polls that instead).
const STATUS_POLL_MS = 1500;
const STALL_THRESHOLD_MS = 4000;

// Checked once on mount, same pattern as the printer profile fetch — if a
// camera gets plugged in later, a page refresh picks it up, no polling
// infrastructure for a secondary feature. `streamFailed` covers the other
// direction: the backend reported a camera at mount time, but the actual
// <img> stream broke afterward (camera unplugged mid-session, backend
// restarted, etc.) — falls back to the same "no camera" placeholder rather
// than showing a broken-image icon.
export function CameraView() {
  const [info, setInfo] = useState({
    checked: false,
    available: false,
    name: "",
  });
  const [streamFailed, setStreamFailed] = useState(false);
  const [fullscreen, setFullscreen] = useState(false);
  const containerRef = useRef(null);

  useEffect(() => {
    let cancelled = false;
    api
      .getCameraInfo()
      .then((data) => {
        if (!cancelled) setInfo({ checked: true, ...data });
      })
      .catch(() => {
        if (!cancelled) setInfo({ checked: true, available: false, name: "" });
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    const handleChange = () =>
      setFullscreen(document.fullscreenElement === containerRef.current);
    document.addEventListener("fullscreenchange", handleChange);
    return () => document.removeEventListener("fullscreenchange", handleChange);
  }, []);

  const toggleFullscreen = () => {
    if (document.fullscreenElement) {
      document.exitFullscreen?.();
    } else {
      containerRef.current?.requestFullscreen?.();
    }
  };

  const showStream = info.available && !streamFailed;

  // "pending" until the first poll, then "live"/"stalled" depending on
  // whether frameSeq has advanced within STALL_THRESHOLD_MS. Tracked in
  // refs (not state) between polls so a run of unchanged polls doesn't
  // need a state update each time — only liveStatus itself, when it
  // actually flips, triggers a render.
  const [liveStatus, setLiveStatus] = useState("pending");
  const lastSeqRef = useRef(null);
  const lastChangeAtRef = useRef(0);

  useEffect(() => {
    if (!showStream) {
      setLiveStatus("pending");
      return;
    }

    let cancelled = false;
    lastSeqRef.current = null;

    const poll = () => {
      api
        .getCameraInfo()
        .then((data) => {
          if (cancelled) return;
          const seq = data.frameSeq ?? 0;
          const now = Date.now();
          if (lastSeqRef.current === null || seq !== lastSeqRef.current) {
            lastSeqRef.current = seq;
            lastChangeAtRef.current = now;
          }
          setLiveStatus(
            now - lastChangeAtRef.current < STALL_THRESHOLD_MS
              ? "live"
              : "stalled"
          );
        })
        .catch(() => {});
    };

    poll();
    const interval = setInterval(poll, STATUS_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [showStream]);

  return (
    <Card>
      <CardContent>
        <div
          ref={containerRef}
          className="relative aspect-video w-full overflow-hidden rounded-lg bg-secondary/40"
        >
          {showStream ? (
            <>
              <img
                src="/api/camera/stream"
                alt="Printer camera stream"
                className="size-full object-contain"
                onError={() => setStreamFailed(true)}
              />
              {info.name && (
                <div className="pointer-events-none absolute inset-x-0 top-0 bg-gradient-to-b from-background/80 to-transparent px-3 py-2 text-xs font-medium text-foreground">
                  {info.name}
                </div>
              )}
              {liveStatus !== "pending" && (
                <div
                  className={cn(
                    "pointer-events-none absolute right-2 top-2 flex items-center gap-1.5 rounded-full border px-2 py-1 text-xs font-medium backdrop-blur-sm",
                    liveStatus === "live"
                      ? "border-primary/40 bg-background/70 text-primary"
                      : "border-amber-500/40 bg-background/70 text-amber-500"
                  )}
                >
                  <span
                    className={cn(
                      "size-1.5 rounded-full",
                      liveStatus === "live"
                        ? "animate-pulse bg-primary"
                        : "bg-amber-500"
                    )}
                  />
                  {liveStatus === "live" ? "Live" : "No new frames"}
                </div>
              )}
              <Button
                variant="secondary"
                size="icon-sm"
                onClick={toggleFullscreen}
                className="absolute bottom-2 right-2 border border-border bg-background/70 backdrop-blur-sm hover:bg-background/90"
              >
                {fullscreen ? (
                  <Minimize className="size-4" />
                ) : (
                  <Maximize className="size-4" />
                )}
              </Button>
            </>
          ) : (
            <div className="flex size-full flex-col items-center justify-center gap-2 text-muted-foreground">
              <CameraOff className="size-8" />
              <span className="text-sm">No camera detected</span>
            </div>
          )}
        </div>
      </CardContent>
    </Card>
  );
}
