import { useEffect, useRef, useState } from "react";
import { CameraOff, Maximize, Minimize, RefreshCw } from "lucide-react";
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

// How long to wait before rebuilding a broken stream. Long enough that a
// camera which is genuinely gone doesn't get hammered with a reconnect
// every frame interval, short enough that replugging one feels immediate.
const RECONNECT_DELAY_MS = 2000;

// The camera is hot-pluggable, which is why this polls GET /api/camera
// continuously rather than checking once on mount: each poll makes the
// backend re-check the /dev/video* node list, so plugging a camera in
// while the dashboard is open picks it up with no refresh. That poll is
// cheap on purpose — the backend only actually opens a device when the
// node list changed, so an idle machine never lights the camera's privacy
// LED. See camera.c's device_nodes.
//
// The same poll drives recovery. An unplugged camera kills the backend's
// capture thread, which ends the MJPEG response and fires the <img>'s
// onError; `streamBroken` records that, and the reconnect effect below
// rebuilds the stream once the backend reports a camera again. It is
// deliberately not a one-way latch — it used to be, which is why a
// replugged camera never came back without a page reload.
export function CameraView() {
  const [info, setInfo] = useState({
    checked: false,
    available: false,
    name: "",
    streaming: false,
  });
  const [streamBroken, setStreamBroken] = useState(false);
  // Changing this remounts the <img> and cache-busts its URL, which is
  // what actually reissues the stream request — React would otherwise
  // reuse the element and the browser would reuse the dead response.
  const [streamKey, setStreamKey] = useState(0);
  const [fullscreen, setFullscreen] = useState(false);
  const containerRef = useRef(null);

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

  const showStream = info.available && !streamBroken;

  // "pending" until the first poll, then "live"/"stalled" depending on
  // whether frameSeq has advanced within STALL_THRESHOLD_MS. Tracked in
  // refs (not state) between polls so a run of unchanged polls doesn't
  // need a state update each time — only liveStatus itself, when it
  // actually flips, triggers a render.
  const [liveStatus, setLiveStatus] = useState("pending");
  const lastSeqRef = useRef(null);
  const lastChangeAtRef = useRef(0);

  // Runs for the whole life of the component, not only while a stream is
  // showing — this is what notices a camera being plugged in.
  useEffect(() => {
    let cancelled = false;

    const poll = () => {
      api
        .getCameraInfo()
        .then((data) => {
          if (cancelled) return;
          setInfo({ checked: true, ...data });

          if (!data.available) {
            // Nothing plugged in: forget any previous failure so a
            // camera appearing later starts from a clean slate instead
            // of inheriting the last one's broken state.
            setStreamBroken(false);
            setLiveStatus("pending");
            lastSeqRef.current = null;
            return;
          }

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
        .catch(() => {
          if (!cancelled) setInfo((current) => ({ ...current, checked: true }));
        });
    };

    poll();
    const interval = setInterval(poll, STATUS_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, []);

  // Rebuilds the stream when there's a camera to rebuild it on. Two ways
  // in: the <img> errored (the usual one — the backend ends the response
  // when its capture thread dies), or frames stopped advancing while the
  // backend says it isn't capturing, which is the same death seen from
  // the other side if the response happens to stay open.
  const needsReconnect =
    info.available &&
    (streamBroken || (liveStatus === "stalled" && info.streaming === false));

  useEffect(() => {
    if (!needsReconnect) return;

    const timer = setTimeout(() => {
      setStreamBroken(false);
      lastSeqRef.current = null;
      lastChangeAtRef.current = Date.now();
      setStreamKey((key) => key + 1);
    }, RECONNECT_DELAY_MS);

    return () => clearTimeout(timer);
  }, [needsReconnect]);

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
                key={streamKey}
                src={`/api/camera/stream?connection=${streamKey}`}
                alt="Printer camera stream"
                className="size-full animate-fade object-contain"
                onError={() => setStreamBroken(true)}
              />
              {info.name && (
                <div className="pointer-events-none absolute inset-x-0 top-0 bg-gradient-to-b from-background/80 to-transparent px-3 py-2 text-xs font-medium text-foreground">
                  {info.name}
                </div>
              )}
              {liveStatus !== "pending" && (
                <div
                  key={liveStatus}
                  className={cn(
                    "pointer-events-none absolute right-2 top-2 flex animate-pop items-center gap-1.5 rounded-full border px-2 py-1 text-xs font-medium backdrop-blur-sm transition-colors duration-300 ease-soft",
                    liveStatus === "live"
                      ? "border-primary/40 bg-background/70 text-primary"
                      : "border-amber-500/40 bg-background/70 text-amber-500"
                  )}
                >
                  <span className="relative flex size-1.5 shrink-0">
                    {liveStatus === "live" && (
                      <span className="absolute inset-0 animate-halo rounded-full bg-primary" />
                    )}
                    <span
                      className={cn(
                        "relative size-1.5 rounded-full",
                        liveStatus === "live" ? "bg-primary" : "bg-amber-500"
                      )}
                    />
                  </span>
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
              {info.available ? (
                <>
                  <RefreshCw className="size-8 animate-spin" />
                  <span className="text-sm">Reconnecting to the camera…</span>
                </>
              ) : (
                <>
                  <CameraOff className="size-8" />
                  <span className="text-sm">No camera detected</span>
                  <span className="max-w-56 text-center text-xs">
                    Plug one in and it&apos;ll appear here — no refresh needed.
                  </span>
                </>
              )}
            </div>
          )}
        </div>
      </CardContent>
    </Card>
  );
}
