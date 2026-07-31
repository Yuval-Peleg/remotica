import { useState } from "react";
import { Flame, Thermometer } from "lucide-react";
import { BedSchematic } from "@/components/control/BedSchematic";
import { AxisRail } from "@/components/control/AxisRail";
import { TempDial } from "@/components/control/TempDial";
import { PRINTER_PROFILE } from "@/lib/printer-profile";
import { TEMP_COLORS } from "@/lib/temp-colors";

// Mock initial state — will come from the real printer connection later.
const INITIAL_POSITION = { x: 110, y: 110 };
const INITIAL_Z = 15;

export function ControlPanel({ hotend, bed, setHotendTarget, setBedTarget }) {
  const [position, setPosition] = useState(INITIAL_POSITION);
  const [zHeight, setZHeight] = useState(INITIAL_Z);
  const [filamentFed, setFilamentFed] = useState(0);

  const canExtrude = hotend.current >= PRINTER_PROFILE.minExtrudeTempC;

  const jogZ = (deltaMm) => {
    setZHeight((z) =>
      Math.min(Math.max(z + deltaMm, 0), PRINTER_PROFILE.maxZMm)
    );
  };

  const jogExtruder = (deltaMm) => {
    if (!canExtrude) return;
    setFilamentFed((f) => f + deltaMm);
  };

  const handleHome = () => {
    setPosition({ x: 0, y: 0 });
    setZHeight(0);
  };

  return (
    <div className="flex flex-col gap-4">
      <div className="flex flex-col gap-6 lg:flex-row lg:items-center">
        <div className="w-full max-w-xs">
          <BedSchematic
            position={position}
            onChange={setPosition}
            onHome={handleHome}
          />
        </div>

        <div className="flex items-center justify-center gap-6">
          <AxisRail
            label="Z"
            valueLabel={`${zHeight.toFixed(1)}mm`}
            onJog={jogZ}
          />
          <AxisRail
            label="E"
            valueLabel={`${filamentFed.toFixed(1)}mm fed`}
            onJog={jogExtruder}
            disabled={!canExtrude}
            disabledReason={`Heat hotend above ${PRINTER_PROFILE.minExtrudeTempC}°C to extrude`}
          />
        </div>

        <div className="flex flex-1 flex-col gap-3 sm:flex-row lg:flex-col">
          <TempDial
            label="Hotend"
            icon={Flame}
            color={TEMP_COLORS.hotend}
            current={hotend.current}
            target={hotend.target}
            onTargetChange={setHotendTarget}
          />
          <TempDial
            label="Bed"
            icon={Thermometer}
            color={TEMP_COLORS.bed}
            current={bed.current}
            target={bed.target}
            onTargetChange={setBedTarget}
            maxTarget={120}
          />
        </div>
      </div>

      {!canExtrude && (
        <p className="text-xs text-muted-foreground">
          Extrude is locked until the hotend reaches{" "}
          {PRINTER_PROFILE.minExtrudeTempC}&deg;C (currently{" "}
          {Math.round(hotend.current)}&deg;C).
        </p>
      )}
    </div>
  );
}
