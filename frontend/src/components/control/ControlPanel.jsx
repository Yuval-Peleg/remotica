import { Flame, Lock, Thermometer } from "lucide-react";
import { BedSchematic } from "@/components/control/BedSchematic";
import { AxisRail } from "@/components/control/AxisRail";
import { TempDial } from "@/components/control/TempDial";
import { TEMP_COLORS } from "@/lib/temp-colors";
import { cn } from "@/lib/utils";

export function ControlPanel({
  hotend,
  bed,
  position,
  jobStatus,
  profile,
  jog,
  home,
  setHotendTarget,
  setBedTarget,
}) {
  const canExtrude = hotend.current >= profile.minExtrudeTempC;
  const isPrintActive = jobStatus === "printing" || jobStatus === "paused";

  const jogZ = (deltaMm) => jog("Z", deltaMm);
  const jogExtruder = (deltaMm) => {
    if (!canExtrude) return;
    jog("E", deltaMm);
  };

  return (
    <div className="relative flex flex-col gap-4">
      <div
        className={cn(
          "flex flex-col gap-6 lg:flex-row lg:items-center",
          isPrintActive && "pointer-events-none opacity-30 blur-[1px]"
        )}
      >
        <div className="mx-auto w-full max-w-xs lg:mx-0">
          <BedSchematic
            position={position}
            bedWidthMm={profile.bedWidthMm}
            bedDepthMm={profile.bedDepthMm}
            onJog={jog}
            onHome={home}
          />
        </div>

        <div className="flex flex-col items-center gap-3">
          <div className="flex items-center justify-center gap-6">
            <AxisRail
              label="Z"
              valueLabel={`${position.z.toFixed(1)}mm`}
              onJog={jogZ}
            />
            <AxisRail
              label="E"
              valueLabel={`${position.e.toFixed(1)}mm fed`}
              onJog={jogExtruder}
              disabled={!canExtrude}
              disabledReason={`Heat hotend above ${profile.minExtrudeTempC}°C to extrude`}
            />
          </div>

          {!canExtrude && (
            <p className="max-w-40 text-center text-xs text-muted-foreground">
              Extrude is locked until the hotend reaches{" "}
              {profile.minExtrudeTempC}&deg;C (currently{" "}
              {Math.round(hotend.current)}&deg;C).
            </p>
          )}
        </div>

        <div className="flex flex-1 flex-col gap-3 sm:flex-row lg:flex-col">
          <TempDial
            label="Hotend"
            icon={Flame}
            color={TEMP_COLORS.hotend}
            current={hotend.current}
            target={hotend.target}
            onTargetChange={setHotendTarget}
            maxTarget={profile.maxHotendTempC}
          />
          <TempDial
            label="Bed"
            icon={Thermometer}
            color={TEMP_COLORS.bed}
            current={bed.current}
            target={bed.target}
            onTargetChange={setBedTarget}
            maxTarget={profile.maxBedTempC}
          />
        </div>
      </div>

      {isPrintActive && (
        <div className="absolute inset-0 z-10 flex flex-col items-center justify-center gap-2 rounded-lg bg-background/70 text-center backdrop-blur-sm">
          <Lock className="size-10 text-muted-foreground" />
          <p className="max-w-56 text-sm text-muted-foreground">
            Can&apos;t manually control the printer mid-print
          </p>
        </div>
      )}
    </div>
  );
}
