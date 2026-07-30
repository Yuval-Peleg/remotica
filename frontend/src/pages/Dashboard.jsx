import { Wifi, WifiOff } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import { ControlPanel } from "@/components/control/ControlPanel";
import { TemperatureGraph } from "@/components/control/TemperatureGraph";
import { usePrinterTemps } from "@/hooks/use-printer-temps";

// Placeholder data — will come from a real printer connection later.
const printData = {
  connected: true,
  state: "Printing",
  file: "benchy.gcode",
  printTime: "1h 12m",
  printTimeLeft: "38m",
  progress: 62,
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
  const { connected, state, file, printTime, printTimeLeft, progress } =
    printData;
  const { hotend, bed, setHotendTarget, setBedTarget, history } =
    usePrinterTemps();

  return (
    <main className="mx-auto grid max-w-7xl gap-6 px-4 py-6 sm:px-6 lg:grid-cols-[minmax(0,360px)_1fr] lg:px-8">
      <section className="flex flex-col gap-4">
        <Badge
          variant={connected ? "default" : "destructive"}
          className="w-fit gap-1.5"
        >
          {connected ? (
            <Wifi className="size-3" />
          ) : (
            <WifiOff className="size-3" />
          )}
          {connected ? "Connected" : "Disconnected"}
        </Badge>

        <Card>
          <CardContent className="flex flex-col gap-4">
            <img
              src="/images/placeholder1.jpeg"
              alt="Print preview"
              className="aspect-square w-full rounded-lg object-cover"
            />

            <div>
              <StatRow label="State" value={state} />
              <Separator />
              <StatRow label="File" value={file} />
              <StatRow label="Print time" value={printTime} />
              <StatRow label="Time left" value={printTimeLeft} />
            </div>

            <div className="flex flex-col gap-1.5">
              <div className="flex items-center justify-between text-xs text-muted-foreground">
                <span>Progress</span>
                <span>{progress}%</span>
              </div>
              <Progress value={progress} />
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
      </section>
    </main>
  );
}
