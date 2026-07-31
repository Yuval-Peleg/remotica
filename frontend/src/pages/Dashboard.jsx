import { useEffect, useState } from "react";
import { Pause, Play, Printer, Square } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import { ControlPanel } from "@/components/control/ControlPanel";
import { TemperatureGraph } from "@/components/control/TemperatureGraph";
import { ConnectionStatus } from "@/components/dashboard/ConnectionStatus";
import { GcodeDropzone } from "@/components/dashboard/GcodeDropzone";
import { OnDeviceFiles } from "@/components/dashboard/OnDeviceFiles";
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
};

const JOB_STATUS_LABELS = {
  idle: "Idle",
  ready: "Ready to print",
  printing: "Printing",
  paused: "Paused",
};

function StatRow({ label, value }) {
  return (
    <div className="flex items-center justify-between py-1.5 text-sm">
      <span className="text-muted-foreground">{label}</span>
      <span className="font-medium text-foreground">{value}</span>
    </div>
  );
}

export function Dashboard() {
  const {
    connected,
    connectionType,
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
    api
      .getProfile()
      .then(setProfile)
      .catch(() => {});
  }, []);

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
  // Time left will be able to come from real progress once printing streams
  // the actual file instead of the backend's placeholder timer — see the
  // comment on job_manager_tick() in the backend for why that's not done
  // yet. Until then this just mirrors the total estimate.
  const printTimeLeft = printTime;

  return (
    <main className="mx-auto grid max-w-7xl gap-6 px-4 py-6 sm:px-6 lg:grid-cols-[minmax(0,360px)_1fr] lg:px-8">
      <section className="flex flex-col gap-4">
        <Card>
          <CardContent className="flex flex-col gap-4">
            <ConnectionStatus
              connected={connected}
              connectionType={connectionType}
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
              <div className="flex gap-2">
                <Button
                  className="flex-1 bg-amber-500 text-amber-950 hover:bg-amber-500/80"
                  onClick={() =>
                    (isPaused ? api.printResume() : api.printPause()).catch(
                      () => {}
                    )
                  }
                >
                  {isPaused ? <Play /> : <Pause />}
                  {isPaused ? "Resume" : "Pause"}
                </Button>
                <Button
                  variant="destructive"
                  className="flex-1"
                  onClick={() => api.printCancel().catch(() => {})}
                >
                  <Square />
                  Stop
                </Button>
              </div>
            ) : (
              <Button
                className="w-full"
                disabled={job.status !== "ready"}
                onClick={() => api.printStart().catch(() => {})}
              >
                <Printer />
                Print
              </Button>
            )}

            <div>
              <StatRow label="State" value={state} />

              <div
                className={cn(
                  "grid transition-[grid-template-rows] duration-300 ease-in-out",
                  hasFile ? "grid-rows-[1fr]" : "grid-rows-[0fr]"
                )}
              >
                <div className="overflow-hidden">
                  <Separator />
                  <StatRow label="File" value={fileLabel} />
                  <StatRow label="Print time" value={printTime} />
                  <StatRow label="Time left" value={printTimeLeft} />

                  <div className="flex flex-col gap-1.5 pt-4">
                    <div className="flex items-center justify-between text-xs text-muted-foreground">
                      <span>Progress</span>
                      <span>{Math.round(job.progress)}%</span>
                    </div>
                    <Progress value={job.progress} />
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

      <section className="flex flex-col gap-4">
        <Card className="overflow-hidden py-0">
          <img
            src="/images/placeholder2.jpg"
            alt="Webcam stream"
            className="aspect-video w-full object-cover"
          />
        </Card>

        <Card>
          <CardContent>
            <ControlPanel
              hotend={hotend}
              bed={bed}
              position={position}
              jobStatus={job.status}
              profile={profile}
              jog={jog}
              home={home}
              setHotendTarget={setHotendTarget}
              setBedTarget={setBedTarget}
            />
          </CardContent>
        </Card>

        <Card>
          <CardContent>
            <TemperatureGraph
              history={history}
              hotendTarget={hotend.target}
              bedTarget={bed.target}
            />
          </CardContent>
        </Card>

        <Card>
          <CardContent>
            <GcodeConsole />
          </CardContent>
        </Card>
      </section>
    </main>
  );
}
