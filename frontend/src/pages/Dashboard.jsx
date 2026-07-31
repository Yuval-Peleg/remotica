import { useState } from "react";
import { Printer } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import { ControlPanel } from "@/components/control/ControlPanel";
import { TemperatureGraph } from "@/components/control/TemperatureGraph";
import { ConnectionStatus } from "@/components/dashboard/ConnectionStatus";
import { GcodeDropzone } from "@/components/dashboard/GcodeDropzone";
import { BackendConnectionTest } from "@/components/dashboard/BackendConnectionTest";
import { usePrinterTemps } from "@/hooks/use-printer-temps";
import { useGcodeFile } from "@/hooks/use-gcode-file";
import { formatDurationShort } from "@/lib/gcode-print-time";
import { cn } from "@/lib/utils";

// Placeholder — will come from a real printer connection later.
const CONNECTION = {
  connected: true,
  connectionType: "usb", // "usb" | "wifi"
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
  const { connected, connectionType } = CONNECTION;
  const { hotend, bed, setHotendTarget, setBedTarget, history } =
    usePrinterTemps();
  const {
    file: queuedFile,
    thumbnail,
    printTimeSeconds,
    error: fileError,
    selectFile,
    clearFile,
  } = useGcodeFile();
  const [isPrinting, setIsPrinting] = useState(false);

  const fileLabel = queuedFile
    ? queuedFile.name.replace(/\.gcode$/i, "")
    : "No file selected";

  const state = !connected
    ? "Disconnected"
    : isPrinting
      ? "Printing"
      : queuedFile
        ? "Ready to print"
        : "Idle";

  const printTime = formatDurationShort(printTimeSeconds);
  // Real progress will come from the printer once a backend exists — for
  // now it's always 0%, so the full estimate is still "remaining".
  const progress = 0;
  const printTimeLeft = printTime;

  const handleClear = () => {
    setIsPrinting(false);
    clearFile();
  };

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
              file={queuedFile}
              thumbnail={thumbnail}
              error={fileError}
              onSelect={selectFile}
              onClear={handleClear}
            />

            <Button
              className="w-full"
              disabled={!queuedFile || isPrinting}
              onClick={() => setIsPrinting(true)}
            >
              <Printer />
              Print
            </Button>

            <div>
              <StatRow label="State" value={state} />

              <div
                className={cn(
                  "grid transition-[grid-template-rows] duration-300 ease-in-out",
                  queuedFile ? "grid-rows-[1fr]" : "grid-rows-[0fr]"
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
                      <span>{progress}%</span>
                    </div>
                    <Progress value={progress} />
                  </div>
                </div>
              </div>
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

        <BackendConnectionTest />
      </section>
    </main>
  );
}
