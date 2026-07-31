import { useState } from "react";
import { Send } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";

// Temporary connectivity smoke test — remove once real controls (jog,
// temps, print) are wired up to talk to the backend for real.
export function BackendConnectionTest() {
  const [message, setMessage] = useState("");
  const [sending, setSending] = useState(false);
  const [log, setLog] = useState([]);

  const send = async () => {
    const text = message.trim();
    if (!text) return;

    setSending(true);
    try {
      const res = await fetch("/api/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ message: text }),
      });
      const data = await res.json();
      setLog((prev) => [{ sent: text, reply: data, ok: res.ok }, ...prev]);
      setMessage("");
    } catch {
      setLog((prev) => [{ sent: text, reply: null, ok: false }, ...prev]);
    } finally {
      setSending(false);
    }
  };

  return (
    <Card className="border-dashed">
      <CardContent className="flex flex-col gap-3">
        <div className="flex items-center justify-between gap-3">
          <p className="text-sm font-medium text-foreground">
            Backend connection test
          </p>
          <span className="text-xs text-muted-foreground">
            dev only — remove later
          </span>
        </div>

        <div className="flex gap-2">
          <Input
            value={message}
            onChange={(e) => setMessage(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && send()}
            placeholder="type something, e.g. hello backend"
          />
          <Button
            size="icon"
            onClick={send}
            disabled={sending || !message.trim()}
          >
            <Send />
          </Button>
        </div>

        {log.length > 0 && (
          <div className="flex flex-col gap-1 font-mono text-xs">
            {log.map((entry, i) => (
              <div
                key={i}
                className={
                  entry.ok ? "text-muted-foreground" : "text-destructive"
                }
              >
                {entry.ok
                  ? `sent "${entry.sent}" → backend acked it`
                  : `sent "${entry.sent}" → no response from backend (is it running?)`}
              </div>
            ))}
          </div>
        )}
      </CardContent>
    </Card>
  );
}
