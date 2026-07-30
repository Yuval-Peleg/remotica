import { useEffect, useState } from "react";

const SAMPLE_INTERVAL_MS = 2000;
const HISTORY_LENGTH = 90; // 3 minutes at one sample every 2s
const APPROACH_RATE = 0.12; // fraction of current->target gap closed per tick
const AMBIENT_C = 22;

function nextTemp(current, target) {
  const drift = (target - current) * APPROACH_RATE;
  const jitter = (Math.random() - 0.5) * 0.3;
  return Math.max(AMBIENT_C, current + drift + jitter);
}

// Mock printer temperature state until a real printer connection exists.
// Current temp drifts toward target each tick (simulating heating/cooling),
// and every tick is also recorded into a rolling history buffer for the
// temperature-over-time graph.
export function usePrinterTemps() {
  const [hotend, setHotend] = useState({ current: 24, target: 0 });
  const [bed, setBed] = useState({ current: 22, target: 0 });
  const [history, setHistory] = useState(() => [
    { t: Date.now(), hotend: 24, bed: 22 },
  ]);

  useEffect(() => {
    const id = setInterval(() => {
      setHotend((h) => ({ ...h, current: nextTemp(h.current, h.target) }));
      setBed((b) => ({ ...b, current: nextTemp(b.current, b.target) }));
    }, SAMPLE_INTERVAL_MS);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    setHistory((h) => {
      const next = [
        ...h,
        { t: Date.now(), hotend: hotend.current, bed: bed.current },
      ];
      return next.length > HISTORY_LENGTH
        ? next.slice(next.length - HISTORY_LENGTH)
        : next;
    });
    // Only the samples themselves should trigger a new history point.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [hotend.current, bed.current]);

  return {
    hotend,
    bed,
    setHotendTarget: (target) => setHotend((h) => ({ ...h, target })),
    setBedTarget: (target) => setBed((b) => ({ ...b, target })),
    history,
  };
}
