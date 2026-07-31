import { useEffect, useRef, useState } from "react";
import { CameraOff, Maximize, Minimize } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { api } from "@/lib/api";

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
