import { useEffect, useState } from "react";
import { toast } from "sonner";
import { api } from "@/lib/api";

const HISTORY_LENGTH = 200; // backend ticks ~every 300ms, so this is ~60s of history
const RECONNECT_DELAY_MS = 2000;

const INITIAL_STATE = {
  connected: false,
  // Assumed homed until the backend says otherwise, so the "home first"
  // notice can't flash on screen during the moment before the first state
  // arrives — it should appear because the printer really isn't homed,
  // not because nothing has been heard yet.
  homed: true,
  connectionType: "usb",
  hotend: { current: 0, target: 0 },
  bed: { current: 0, target: 0 },
  position: { x: 0, y: 0, z: 0, e: 0 },
  job: { status: "idle", filename: "", progress: 0 },
};

// Live printer state, sourced from the real backend: an initial REST fetch
// for a fast first paint, then a WebSocket subscription for continuous
// live updates (temps, position, job progress) — see CLAUDE.md's "Backend"
// section for why the API is REST+WebSocket instead of one or the other.
export function usePrinterState() {
  const [state, setState] = useState(INITIAL_STATE);
  const [history, setHistory] = useState([]);

  useEffect(() => {
    let cancelled = false;
    let socket = null;
    let reconnectTimer = null;

    const applyStateUpdate = (next) => {
      setState(next);
      setHistory((prev) => {
        const point = {
          t: Date.now(),
          hotend: next.hotend.current,
          bed: next.bed.current,
        };
        const updated = [...prev, point];
        return updated.length > HISTORY_LENGTH
          ? updated.slice(updated.length - HISTORY_LENGTH)
          : updated;
      });
    };

    const connectSocket = () => {
      if (cancelled) return;

      const wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${wsProtocol}//${window.location.host}/api/ws`);

      socket.onmessage = (event) => {
        applyStateUpdate(JSON.parse(event.data));
      };

      // If the connection drops (backend restarted, network blip, the PC
      // running the backend went to sleep, ...), keep retrying instead of
      // silently leaving the UI stuck showing stale data forever.
      socket.onclose = () => {
        if (cancelled) return;
        reconnectTimer = setTimeout(connectSocket, RECONNECT_DELAY_MS);
      };
    };

    // Fast initial paint before the WebSocket connects.
    api
      .getState()
      .then((initial) => {
        if (!cancelled) setState(initial);
      })
      .catch(() => {
        /* The WebSocket connecting below will catch us up once it's live. */
      });

    connectSocket();

    return () => {
      cancelled = true;
      clearTimeout(reconnectTimer);
      socket?.close();
    };
  }, []);

  // Jog/home/temp all end up driving real motors/heaters — refusing them
  // client-side while disconnected (rather than letting them hit the
  // backend and come back as a 502) means an immediate, specific "the
  // printer is disconnected" message instead of a generic failure, and
  // one less pointless round trip. A shared `id` makes repeated attempts
  // (e.g. mashing a jog button) refresh the same toast instead of
  // stacking up duplicates.
  const requireConnected = (action) => {
    if (!state.connected) {
      toast.error("Printer is disconnected", {
        id: "printer-disconnected",
        description: "Connect a printer before trying to control it.",
      });
      return Promise.resolve();
    }
    return action();
  };

  return {
    ...state,
    history,
    jog: (axis, deltaMm) => requireConnected(() => api.jog(axis, deltaMm)),
    home: () => requireConnected(() => api.home()),
    setHotendTarget: (celsius) =>
      requireConnected(() => api.setTemp("hotend", celsius)),
    setBedTarget: (celsius) =>
      requireConnected(() => api.setTemp("bed", celsius)),
  };
}
