import { useEffect, useState } from "react";
import { api } from "@/lib/api";

const HISTORY_LENGTH = 90; // matches the backend's ~300ms tick rate: ~27s of history
const RECONNECT_DELAY_MS = 2000;

const INITIAL_STATE = {
  connected: false,
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

  return {
    ...state,
    history,
    jog: (axis, deltaMm) => api.jog(axis, deltaMm),
    home: () => api.home(),
    setHotendTarget: (celsius) => api.setTemp("hotend", celsius),
    setBedTarget: (celsius) => api.setTemp("bed", celsius),
  };
}
