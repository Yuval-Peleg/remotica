import { useEffect, useRef, useState } from "react";
import { toast } from "sonner";
import { CheckCircle2, Pause, Play, Printer, Square } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { ControlPanel } from "@/components/control/ControlPanel";
import { CameraView } from "@/components/dashboard/CameraView";
import { TemperatureGraph } from "@/components/control/TemperatureGraph";
import { ConnectionStatus } from "@/components/dashboard/ConnectionStatus";
import { GcodeDropzone } from "@/components/dashboard/GcodeDropzone";
import { OnDeviceFiles } from "@/components/dashboard/OnDeviceFiles";
import { HoldButton } from "@/components/dashboard/HoldButton";
import { GcodeConsole } from "@/components/dashboard/GcodeConsole";
import { usePrinterState } from "@/hooks/use-printer-state";
import { useGcodeFile } from "@/hooks/use-gcode-file";
import { useFilePreview } from "@/hooks/use-file-preview";
import { api } from "@/lib/api";
import { formatDurationShort } from "@/lib/gcode-print-time";
import { cn } from "@/lib/utils";

// Used only until GET /api/profile resolves on mount, so the control panel
// has sane numbers to render for the first frame instead of nothing.
const DEFAULT_PROFILE = {
  bedWidthMm: 220,
  bedDepthMm: 220,
  maxZMm: 250,
  minExtrudeTempC: 170,
  maxHotendTempC: 280,
  maxBedTempC: 120,
  // Fail-closed: until the real fetch below resolves, we genuinely don't
  // know if the backend has a configured profile, so ControlPanel should
  // briefly show the "choose a printer" gate rather than briefly show
  // live controls.
  source: "default",
  printerName: "",
};

const JOB_STATUS_LABELS = {
  idle: "Idle",
  ready: "Ready to print",
  printing: "Printing",
  paused: "Paused",
};

// `title` is what a long value falls back to on hover, since the value
// itself is truncated to one line — gcode filenames off a slicer are
// routinely long enough ("CE3_bracket_v4_0.2mm_PLA_MK3S_4h12m.gcode") to
// otherwise blow the row's layout apart.
function StatRow({ label, value, title }) {
  return (
    <div className="flex items-center justify-between gap-3 py-1.5 text-sm">
      <span className="shrink-0 text-muted-foreground">{label}</span>
      <span
        className="min-w-0 truncate font-medium text-foreground"
        title={title}
      >
        {value}
      </span>
    </div>
  );
}

export function Dashboard() {
  const {
    connected,
    homed,
    connectionType,
    firmwareInfo,
    hotend,
    bed,
    position,
    job,
    history,
    jog,
    home,
    setHotendTarget,
    setBedTarget,
  } = usePrinterState();

  const [profile, setProfile] = useState(DEFAULT_PROFILE);
  useEffect(() => {
    // Re-fires on every reconnect (not just on mount) so a page already
    // open when the backend finishes an async auto-detect doesn't stay
    // stuck showing a stale/unconfigured profile.
    if (!connected) return;
    api
      .getProfile()
      .then(setProfile)
      .catch(() => {});
  }, [connected]);

  const { error: uploadError, selectFile } = useGcodeFile();
  const { thumbnail, printTimeSeconds } = useFilePreview(job.filename || null);

  const hasFile = job.status !== "idle";
  const isPaused = job.status === "paused";
  const isPrintActive = job.status === "printing" || isPaused;

  const fileLabel = hasFile
    ? job.filename.replace(/\.gcode$/i, "")
    : "No file selected";
  const state = !connected ? "Disconnected" : JOB_STATUS_LABELS[job.status];

  const printTime = formatDurationShort(printTimeSeconds);
  // Deliberately not formatDurationShort's own output here: it returns the
  // string "Unknown" rather than null when it can't estimate, which would
  // render as "Progress (Time left: Unknown)" — noise next to a bar that
  // is perfectly readable on its own. Known-ness is decided before
  // formatting so the whole parenthetical can be dropped.
  const printTimeLeft =
    printTimeSeconds != null
      ? formatDurationShort(
          Math.max(0, printTimeSeconds * (1 - job.progress / 100))
        )
      : null;

  // Print-finished popup: fires once per completion, when the job goes
  // printing -> ready with progress essentially at 100%. A driver failure
  // also lands on "ready" (see job_manager.c), but leaves progress short of
  // 100 in all but the last-line edge case, so this doesn't fire for that.
  const [showFinishedDialog, setShowFinishedDialog] = useState(false);
  const wasPrintingRef = useRef(false);
  useEffect(() => {
    const isPrinting = job.status === "printing" || job.status === "paused";
    if (
      wasPrintingRef.current &&
      !isPrinting &&
      job.status === "ready" &&
      job.progress >= 99.9
    ) {
      setShowFinishedDialog(true);
    }
    wasPrintingRef.current = isPrinting;
  }, [job.status, job.progress]);

  return (
    <main className="mx-auto grid max-w-7xl gap-6 px-4 py-6 sm:px-6 lg:grid-cols-[minmax(0,360px)_1fr] lg:px-8">
      <section className="flex motion-stagger flex-col gap-4">
        <Card>
          <CardContent className="flex flex-col gap-4">
            <ConnectionStatus
              connected={connected}
              connectionType={connectionType}
              firmwareInfo={firmwareInfo}
            />

            <GcodeDropzone
              filename={job.filename || null}
              thumbnail={thumbnail}
              error={uploadError}
              locked={isPrintActive}
              onSelect={selectFile}
              onClear={() => api.printCancel().catch(() => {})}
            />

            {isPrintActive ? (
              /* Hold-to-confirm, not plain taps: these sit directly in the
                 scroll path on a phone, and brushing one mid-flick would
                 pause or abandon a running print. A hold can't be produced
                 by a moving finger — see HoldButton — and stays faster
                 than a confirmation dialog for the emergency stop this has
                 to remain good at. */
              <div className="flex flex-col gap-1.5">
                <div className="flex animate-pop gap-2">
                  <HoldButton
                    className="flex-1 bg-amber-500 text-amber-950 hover:bg-amber-500/80"
                    holdLabel={isPaused ? "Resuming…" : "Pausing…"}
                    onConfirm={() =>
                      (isPaused ? api.printResume() : api.printPause()).catch(
                        () => {}
                      )
                    }
                  >
                    {isPaused ? <Play /> : <Pause />}
                    {isPaused ? "Resume" : "Pause"}
                  </HoldButton>
                  <HoldButton
                    variant="destructive"
                    className="flex-1"
                    holdLabel="Stopping…"
                    onConfirm={() => api.printCancel().catch(() => {})}
                  >
                    <Square />
                    Stop
                  </HoldButton>
                </div>
                <p className="text-center text-xs text-muted-foreground">
                  Hold either button to confirm
                </p>
              </div>
            ) : (
              <Button
                className="w-full animate-pop"
                disabled={job.status !== "ready"}
                onClick={() => {
                  // job.status can still be "ready" from before a
                  // disconnect (it's independent of connection state),
                  // so the button itself doesn't already rule this out —
                  // check explicitly rather than letting it hit the
                  // backend and come back as a 502.
                  if (!connected) {
                    toast.error("Printer is disconnected", {
                      id: "printer-disconnected",
                      description: "Connect a printer before starting a print.",
                    });
                    return;
                  }
                  api.printStart().catch(() => {});
                }}
              >
                <Printer />
                Print
              </Button>
            )}

            <div>
              <StatRow label="State" value={state} />

              <div
                className={cn(
                  "grid transition-[grid-template-rows] duration-400 ease-smooth",
                  hasFile ? "grid-rows-[1fr]" : "grid-rows-[0fr]"
                )}
              >
                <div className="overflow-hidden">
                  <Separator />
                  <StatRow
                    label="File"
                    value={fileLabel}
                    title={hasFile ? job.filename : undefined}
                  />
                  <StatRow label="Print time" value={printTime} />

                  <div className="flex flex-col gap-1.5 pt-4">
                    <div className="flex items-center justify-between gap-3 text-xs text-muted-foreground">
                      {/* Time remaining reads better attached to the bar
                          it describes than as its own stat row. The
                          parenthetical is dropped entirely when the
                          estimate is unknown — "(Time left: Unknown)"
                          is noise, and the bar is still meaningful
                          without it. */}
                      <span className="min-w-0 truncate">
                        Progress
                        {printTimeLeft ? ` (Time left: ${printTimeLeft})` : ""}
                      </span>
                      <span className="tabular-nums">
                        {Math.round(job.progress)}%
                      </span>
                    </div>
                    <Progress
                      value={job.progress}
                      active={job.status === "printing"}
                    />
                  </div>
                </div>
              </div>

              <OnDeviceFiles
                currentFilename={job.filename}
                printActive={isPrintActive}
                refreshTrigger={job.filename}
              />
            </div>
          </CardContent>
        </Card>
      </section>

      <section className="flex motion-stagger flex-col gap-4">
        <CameraView />

        <Card>
          <CardContent>
            <ControlPanel
              hotend={hotend}
              bed={bed}
              position={position}
              jobStatus={job.status}
              profile={profile}
              connected={connected}
              homed={homed}
              jog={jog}
              home={home}
              setHotendTarget={setHotendTarget}
              setBedTarget={setBedTarget}
            />
          </CardContent>
        </Card>

        <Card>
          <CardContent>
            <TemperatureGraph history={history} profile={profile} />
          </CardContent>
        </Card>

        <Card>
          <CardContent>
            <GcodeConsole />
          </CardContent>
        </Card>
      </section>

      <Dialog open={showFinishedDialog} onOpenChange={setShowFinishedDialog}>
        <DialogContent className="sm:max-w-md">
          <DialogHeader className="items-center text-center">
            <CheckCircle2 className="size-12 animate-pop text-primary" />
            <DialogTitle className="text-lg">Print finished</DialogTitle>
            {/* Wrapped rather than truncated: this is a one-off dialog
                with room to spare, and a filename is the only thing
                identifying which print just finished. */}
            <DialogDescription className="break-words">
              {fileLabel} finished printing.
            </DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button
              className="w-full"
              onClick={() => setShowFinishedDialog(false)}
            >
              OK
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </main>
  );
}
