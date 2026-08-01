import { memo, useEffect, useRef, useState } from "react";
import { ArrowDown } from "lucide-react";
import { useGcodeConsole } from "@/hooks/use-gcode-console";
import { translateGcodeLine } from "@/lib/gcode-translate";
import { Switch } from "@/components/ui/switch";
import { cn } from "@/lib/utils";

// How close to the bottom (in pixels) counts as "still at the bottom" for
// deciding whether to keep following new lines — lets a small amount of
// imprecision (e.g. from a fresh entry changing the scroll height
// slightly) still count as "the user is at the bottom", without needing
// an exact pixel-perfect match.
const AUTO_SCROLL_THRESHOLD_PX = 40;

function formatTime(timestampMs) {
  return new Date(timestampMs).toLocaleTimeString([], { hour12: false });
}

// Memoized per line, and deliberately so. Entry objects are created once in
// useGcodeConsole and never mutated, so every already-visible line's props
// are referentially identical on the next render and React can skip it
// entirely. Without that, appending a single line re-ran this body for all
// MAX_ENTRIES rows — 500 toLocaleTimeString calls (Intl formatting, the
// single most expensive thing here) plus 1000 cn()/tailwind-merge calls —
// which measured ~89ms per render while a print was streaming lines.
const ConsoleLine = memo(function ConsoleLine({ entry, plainEnglish }) {
  const isSent = entry.direction === "sent";
  const translated = plainEnglish
    ? translateGcodeLine(entry.text, entry.direction)
    : null;

  return (
    <div className="flex gap-2 py-0.5 leading-relaxed">
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
});

// Memoized as a whole too: it takes no props and owns its own data, but it
// sits inside Dashboard, which re-renders on every ~300ms printer-state
// tick. Without this, each of those ticks rebuilt the whole line list even
// when no new console line had arrived at all.
export const GcodeConsole = memo(function GcodeConsole() {
  const entries = useGcodeConsole();
  const [plainEnglish, setPlainEnglish] = useState(false);
  const [isFollowing, setIsFollowing] = useState(true);
  const scrollRef = useRef(null);

  // Auto-following writes scrollTop, the browser answers with a scroll
  // event, and handleScroll then re-set isFollowing to the value it
  // already held. React's same-value bailout doesn't reliably skip the
  // render pass while other updates are already pending, so during a print
  // that roughly doubled this component's render count for no visible
  // change. This ref is the only writer of isFollowing, so the two cannot
  // drift apart.
  const followingRef = useRef(true);
  const setFollowing = (next) => {
    if (followingRef.current === next) return;
    followingRef.current = next;
    setIsFollowing(next);
  };

  // While following, keep pinned to the newest line as entries arrive.
  // Scrolling up by hand (see handleScroll) turns following off, and new
  // lines then arrive without moving the view — exactly like a terminal
  // or chat app that stops "auto-scrolling" once you've scrolled away
  // from the bottom, until you either scroll back down yourself or hit
  // the "jump to latest" button.
  useEffect(() => {
    const el = scrollRef.current;
    if (!el || !isFollowing) return;
    el.scrollTop = el.scrollHeight;
  }, [entries, isFollowing]);

  const handleScroll = () => {
    const el = scrollRef.current;
    if (!el) return;
    const distanceFromBottom = el.scrollHeight - el.scrollTop - el.clientHeight;
    setFollowing(distanceFromBottom < AUTO_SCROLL_THRESHOLD_PX);
  };

  const jumpToLatest = () => {
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight;
    setFollowing(true);
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

      <div className="relative">
        <div
          ref={scrollRef}
          onScroll={handleScroll}
          className="h-64 overflow-y-auto rounded-lg border border-border bg-secondary/20 p-3 font-mono text-xs"
        >
          {entries.length === 0 ? (
            <p className="text-muted-foreground">
              No printer communication yet.
            </p>
          ) : (
            entries.map((entry) => (
              <ConsoleLine
                key={entry.id}
                entry={entry}
                plainEnglish={plainEnglish}
              />
            ))
          )}
        </div>

        {!isFollowing && (
          <button
            type="button"
            onClick={jumpToLatest}
            className="absolute bottom-2 left-1/2 flex -translate-x-1/2 items-center gap-1 rounded-full border border-border bg-card px-3 py-1 text-xs font-medium text-foreground shadow-sm transition-colors hover:bg-secondary"
          >
            <ArrowDown className="size-3" />
            Jump to latest
          </button>
        )}
      </div>
    </div>
  );
});
