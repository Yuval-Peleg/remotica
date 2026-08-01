import { startTransition, useEffect, useRef, useState } from "react";
import { api } from "@/lib/api";

// Cap on how many lines are kept in the browser — the backend's own ring
// buffer (CONSOLE_LOG_CAPACITY in console_log.h) already limits how far
// back GET /api/console can go; this just keeps the live-growing frontend
// list from growing forever during a long session.
const MAX_ENTRIES = 500;
const RECONNECT_DELAY_MS = 2000;

// Floor on the gap between two flushes into React state. Rendering the log
// faster than this is work nobody can perceive — 100ms is still under the
// threshold where a human reads a terminal as "instant" — but each flush
// costs O(MAX_ENTRIES) regardless of how few lines it carries, because the
// list is rebuilt from a new array. Flushing per animation frame meant
// ~60 of those per second (measured at ~99/s once the auto-scroll's own
// feedback renders are counted), which on this dev machine was ~19% of the
// main thread and on a phone or tablet — how this dashboard is actually
// meant to be used, from another device on the LAN — is several times that,
// i.e. right back to a saturated main thread.
const MIN_FLUSH_INTERVAL_MS = 100;

// Live gcode communication log: an initial GET /api/console fetch for the
// recent backlog, then a /api/ws/console subscription that appends each
// new line the instant it's sent/received (pushed per-event, not on the
// 300ms state-tick like usePrinterState — a terminal should feel
// immediate). Same auto-reconnect approach as usePrinterState if the
// connection drops.
export function useGcodeConsole() {
  const [entries, setEntries] = useState([]);

  // A real (or artificially-delayed simulated) print can push dozens of
  // console lines per second — calling setState per message let a fast
  // print's console traffic re-render this list (and starve the rest of
  // the page, including route changes) far more often than the screen
  // can even show. Incoming entries are buffered in a ref and flushed to
  // React state at most once per animation frame instead, which still
  // feels instant (nothing waits longer than a frame) but caps the
  // render rate to what's actually visible.
  const pendingRef = useRef([]);
  const flushHandleRef = useRef(null);
  const lastFlushRef = useRef(0);

  // Every entry gets a stable, ever-increasing id the moment it's first
  // seen here, used as GcodeConsole's list key instead of its array index.
  // That distinction matters a lot once MAX_ENTRIES starts trimming from
  // the front: with an index key, every remaining row's index shifts on
  // each trim, so React can't tell old rows from new ones and re-renders
  // the entire list every flush. A stable id lets it recognize the rows
  // it's already rendered and only touch the ones actually new.
  const nextIdRef = useRef(0);
  const withId = (entry) => ({ ...entry, id: nextIdRef.current++ });

  useEffect(() => {
    let cancelled = false;
    let socket = null;
    let reconnectTimer = null;

    // Still driven by requestAnimationFrame rather than a timer, so a
    // backgrounded tab stops rendering the log entirely instead of piling
    // up work behind a hidden page; the elapsed check just re-arms until
    // MIN_FLUSH_INTERVAL_MS has actually passed.
    const flushPending = () => {
      const now = performance.now();
      if (now - lastFlushRef.current < MIN_FLUSH_INTERVAL_MS) {
        flushHandleRef.current = requestAnimationFrame(flushPending);
        return;
      }
      lastFlushRef.current = now;
      flushHandleRef.current = null;
      if (pendingRef.current.length === 0) return;
      const batch = pendingRef.current;
      pendingRef.current = [];
      // Marked as a transition, which is the part that actually keeps the
      // rest of the app usable during a print. react-router's BrowserRouter
      // wraps its location update in startTransition, and React always
      // renders the highest-priority pending lane — so an ordinary setState
      // arriving here on every flush outranks a route change indefinitely,
      // and clicking a navbar link did nothing at all while the URL had
      // already changed underneath it. Appending log lines genuinely is
      // background work, so putting it on the same lane class as the
      // navigation lets React interleave the two fairly instead of letting
      // the console win every time.
      startTransition(() => {
        setEntries((prev) => {
          const updated = [...prev, ...batch];
          return updated.length > MAX_ENTRIES
            ? updated.slice(updated.length - MAX_ENTRIES)
            : updated;
        });
      });
    };

    const appendEntry = (entry) => {
      pendingRef.current.push(withId(entry));
      if (flushHandleRef.current == null) {
        flushHandleRef.current = requestAnimationFrame(flushPending);
      }
    };

    const connectSocket = () => {
      if (cancelled) return;

      const wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(
        `${wsProtocol}//${window.location.host}/api/ws/console`
      );

      socket.onmessage = (event) => {
        appendEntry(JSON.parse(event.data));
      };

      socket.onclose = () => {
        if (cancelled) return;
        reconnectTimer = setTimeout(connectSocket, RECONNECT_DELAY_MS);
      };
    };

    api
      .getConsoleBacklog()
      .then((backlog) => {
        if (!cancelled) setEntries(backlog.slice(-MAX_ENTRIES).map(withId));
      })
      .catch(() => {
        /* The WebSocket connecting below will still show new lines even
         * if the initial backlog fetch failed. */
      });

    connectSocket();

    return () => {
      cancelled = true;
      clearTimeout(reconnectTimer);
      socket?.close();
      if (flushHandleRef.current != null) {
        cancelAnimationFrame(flushHandleRef.current);
        flushHandleRef.current = null;
      }
    };
  }, []);

  return entries;
}
