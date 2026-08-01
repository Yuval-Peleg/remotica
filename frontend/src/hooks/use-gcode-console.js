import { useEffect, useRef, useState } from "react";
import { api } from "@/lib/api";

// Cap on how many lines are kept in the browser — the backend's own ring
// buffer (CONSOLE_LOG_CAPACITY in console_log.h) already limits how far
// back GET /api/console can go; this just keeps the live-growing frontend
// list from growing forever during a long session.
const MAX_ENTRIES = 500;
const RECONNECT_DELAY_MS = 2000;

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

  useEffect(() => {
    let cancelled = false;
    let socket = null;
    let reconnectTimer = null;

    const flushPending = () => {
      flushHandleRef.current = null;
      if (pendingRef.current.length === 0) return;
      const batch = pendingRef.current;
      pendingRef.current = [];
      setEntries((prev) => {
        const updated = [...prev, ...batch];
        return updated.length > MAX_ENTRIES
          ? updated.slice(updated.length - MAX_ENTRIES)
          : updated;
      });
    };

    const appendEntry = (entry) => {
      pendingRef.current.push(entry);
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
        if (!cancelled) setEntries(backlog.slice(-MAX_ENTRIES));
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
