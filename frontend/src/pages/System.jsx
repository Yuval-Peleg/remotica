import { useCallback, useEffect, useState } from "react";
import { toast } from "sonner";
import { Cpu, FolderOpen, HardDrive, Power, Timer, Usb } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Switch } from "@/components/ui/switch";
import { api } from "@/lib/api";

// Uptime advances on its own, so the page re-reads rather than showing a
// number frozen at whenever the tab was opened. Deliberately much slower
// than the printer's 300ms tick — nothing here changes fast, and this
// endpoint shells out to systemctl.
const REFRESH_INTERVAL_MS = 5000;

function formatUptime(totalSeconds) {
  if (!Number.isFinite(totalSeconds) || totalSeconds < 0) return "—";

  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);

  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m`;
  return `${Math.floor(totalSeconds)}s`;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return "Unknown";

  const gb = bytes / 1024 ** 3;
  if (gb >= 1) return `${gb.toFixed(1)} GB free`;
  return `${Math.round(bytes / 1024 ** 2)} MB free`;
}

function StatRow({ icon: Icon, label, value, mono = false }) {
  return (
    <div className="flex items-start justify-between gap-4 py-3">
      <span className="flex items-center gap-2.5 text-sm text-muted-foreground">
        <Icon className="size-4 shrink-0" />
        {label}
      </span>
      <span
        className={
          mono
            ? "text-right font-mono text-sm break-all text-foreground"
            : "text-right text-sm text-foreground"
        }
      >
        {value}
      </span>
    </div>
  );
}

export function System() {
  const [info, setInfo] = useState(null);
  const [failed, setFailed] = useState(false);
  const [saving, setSaving] = useState(false);

  const refresh = useCallback(async () => {
    try {
      setInfo(await api.getSystem());
      setFailed(false);
    } catch {
      setFailed(true);
    }
  }, []);

  useEffect(() => {
    refresh();
    const timer = setInterval(refresh, REFRESH_INTERVAL_MS);
    return () => clearInterval(timer);
  }, [refresh]);

  const handleBootStartChange = async (next) => {
    // Optimistic, then reconciled by the refresh below — without this the
    // switch sits at its old position for a whole systemctl round trip and
    // reads as an unresponsive control.
    setInfo((current) => ({ ...current, bootStartEnabled: next }));
    setSaving(true);
    try {
      await api.setBootStart(next);
      toast.success(
        next
          ? "Remotica will start when this machine boots"
          : "Remotica will no longer start on boot"
      );
    } catch (error) {
      setInfo((current) => ({ ...current, bootStartEnabled: !next }));
      toast.error(error.message || "Couldn't change the boot setting");
    } finally {
      setSaving(false);
      refresh();
    }
  };

  return (
    <main className="mx-auto flex max-w-3xl flex-col gap-8 px-4 py-10 sm:px-6 lg:px-8">
      <header className="flex animate-rise flex-col gap-1">
        <h1 className="font-heading text-2xl font-semibold text-foreground">
          System
        </h1>
        <p className="text-sm text-muted-foreground">
          What this machine is running, and whether it comes back on its own
          after a power cut.
        </p>
      </header>

      {failed && info === null ? (
        <Card>
          <CardContent>
            <p className="text-sm text-muted-foreground">
              Couldn&apos;t reach the backend to read system status.
            </p>
          </CardContent>
        </Card>
      ) : info === null ? (
        <Card>
          <CardContent className="flex flex-col gap-3">
            {[0, 1, 2, 3, 4].map((row) => (
              <div
                key={row}
                className="h-6 w-full animate-pulse rounded-md bg-muted"
              />
            ))}
          </CardContent>
        </Card>
      ) : (
        <div className="flex motion-stagger flex-col gap-4">
          <Card>
            <CardContent className="divide-y divide-border py-0">
              <StatRow icon={Cpu} label="Version" value={info.version} mono />
              <StatRow
                icon={Timer}
                label="Uptime"
                value={formatUptime(info.uptimeSeconds)}
              />
              <StatRow
                icon={Usb}
                label="Serial port"
                value={info.serialPort}
                mono
              />
              <StatRow
                icon={FolderOpen}
                label="Data directory"
                value={info.dataDir}
                mono
              />
              <StatRow
                icon={HardDrive}
                label="Storage"
                value={formatBytes(info.dataFreeBytes)}
              />
            </CardContent>
          </Card>

          <Card>
            <CardContent className="flex flex-col gap-3">
              <div className="flex items-start justify-between gap-6">
                <div className="flex flex-col gap-1">
                  <span className="flex items-center gap-2.5 text-sm font-medium text-foreground">
                    <Power className="size-4 shrink-0" />
                    Start on boot
                  </span>
                  <p className="max-w-md text-sm text-muted-foreground">
                    {info.managed
                      ? "Remotica starts by itself when this machine powers on, so you don't need to log in or open a terminal."
                      : "Remotica is running from a source checkout, not as an installed service, so there's no boot behaviour to change. Install it with the script in the README to manage this from here."}
                  </p>
                </div>
                <Switch
                  checked={info.managed && info.bootStartEnabled === true}
                  onCheckedChange={handleBootStartChange}
                  disabled={!info.managed || saving}
                  aria-label="Start Remotica when this machine boots"
                />
              </div>
            </CardContent>
          </Card>
        </div>
      )}
    </main>
  );
}
