import { useEffect, useRef, useState } from "react";
import { useGcodeConsole } from "@/hooks/use-gcode-console";
import { translateGcodeLine } from "@/lib/gcode-translate";
import { Switch } from "@/components/ui/switch";
import { cn } from "@/lib/utils";

// How close to the bottom (in pixels) counts as "still at the bottom" for
// deciding whether to auto-scroll — lets a small amount of imprecision
// (e.g. from a fresh entry changing the scroll height slightly) still
// count as "the user was following along", without needing an exact
// pixel-perfect match.
const AUTO_SCROLL_THRESHOLD_PX = 40;

function formatTime(timestampMs) {
  return new Date(timestampMs).toLocaleTimeString([], { hour12: false });
}

export function GcodeConsole() {
  const entries = useGcodeConsole();
  const [plainEnglish, setPlainEnglish] = useState(false);
  const scrollRef = useRef(null);
  const shouldAutoScrollRef = useRef(true);

  // Auto-scroll to the newest line as entries arrive — but only if the
  // user was already at (or near) the bottom. If they've scrolled up to
  // read earlier history, new lines shouldn't yank the view back down
  // out from under them.
  useEffect(() => {
    const el = scrollRef.current;
    if (!el || !shouldAutoScrollRef.current) return;
    el.scrollTop = el.scrollHeight;
  }, [entries]);

  const handleScroll = () => {
    const el = scrollRef.current;
    if (!el) return;
    const distanceFromBottom = el.scrollHeight - el.scrollTop - el.clientHeight;
    shouldAutoScrollRef.current = distanceFromBottom < AUTO_SCROLL_THRESHOLD_PX;
  };

  return (
    <div className="flex flex-col gap-3">
      <div className="flex items-center justify-between">
        <p className="text-sm font-medium text-foreground">Printer console</p>
        <label className="flex items-center gap-2 text-xs text-muted-foreground">
          Plain English
          <Switch
            checked={plainEnglish}
            onCheckedChange={setPlainEnglish}
            size="sm"
          />
        </label>
      </div>

      <div
        ref={scrollRef}
        onScroll={handleScroll}
        className="h-64 overflow-y-auto rounded-lg border border-border bg-secondary/20 p-3 font-mono text-xs"
      >
        {entries.length === 0 ? (
          <p className="text-muted-foreground">No printer communication yet.</p>
        ) : (
          entries.map((entry, index) => {
            const isSent = entry.direction === "sent";
            const translated = plainEnglish
              ? translateGcodeLine(entry.text, entry.direction)
              : null;

            return (
              <div key={index} className="flex gap-2 py-0.5 leading-relaxed">
                <span className="shrink-0 text-muted-foreground/60">
                  {formatTime(entry.timestampMs)}
                </span>
                <span
                  className={cn(
                    "shrink-0",
                    isSent ? "text-primary" : "text-muted-foreground"
                  )}
                >
                  {isSent ? "→" : "←"}
                </span>
                <span
                  className={cn(
                    "break-all",
                    isSent ? "text-foreground" : "text-muted-foreground"
                  )}
                >
                  {translated ?? entry.text}
                </span>
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}
