import { useState } from "react";
import { Link } from "react-router-dom";
import {
  Flame,
  Home,
  Lock,
  Settings as SettingsIcon,
  Thermometer,
} from "lucide-react";
import { BedSchematic } from "@/components/control/BedSchematic";
import { AxisRail } from "@/components/control/AxisRail";
import { TempDial } from "@/components/control/TempDial";
import { Button } from "@/components/ui/button";
import { TEMP_COLORS } from "@/lib/temp-colors";
import { cn } from "@/lib/utils";

export function ControlPanel({
  hotend,
  bed,
  position,
  jobStatus,
  profile,
  connected,
  homed,
  jog,
  home,
  setHotendTarget,
  setBedTarget,
}) {
  const canExtrude = hotend.current >= profile.minExtrudeTempC;
  const isPrintActive = jobStatus === "printing" || jobStatus === "paused";
  // Only a human pressing "Use this printer" counts. An auto-detected
  // match is a proposal, not a confirmation — it names a model, which
  // isn't a promise about bed size or temperature limits on a machine
  // that may have been modified. The backend enforces the same rule, so
  // this isn't the only thing standing between a guess and the hardware.
  const isProfileConfigured = profile.source === "manual";

  const [homing, setHoming] = useState(false);
  const handleHome = async () => {
    setHoming(true);
    try {
      await home();
    } finally {
      setHoming(false);
    }
  };

  const jogZ = (deltaMm) => jog("Z", deltaMm);
  const jogExtruder = (deltaMm) => {
    if (!canExtrude) return;
    jog("E", deltaMm);
  };

  // Not a Remotica restriction — Marlin and most other firmware refuse
  // ordinary moves until a G28 has run, because before that the printer
  // genuinely doesn't know where the head is. Without saying so, a jog
  // just silently does nothing and looks like Remotica being broken. The
  // controls are deliberately left enabled: the firmware is the authority
  // here, and some machines will happily move without homing.
  const needsHoming =
    connected && isProfileConfigured && !isPrintActive && !homed;

  return (
    <div className="relative flex flex-col gap-4">
      {needsHoming && (
        <div className="flex animate-rise flex-wrap items-center justify-between gap-3 rounded-lg border border-amber-500/40 bg-amber-500/10 px-3 py-2">
          <p className="text-xs text-amber-200">
            Home the printer before moving it — until then it doesn&apos;t know
            where the head is, and most firmware refuses to move.
          </p>
          <Button
            size="sm"
            variant="secondary"
            onClick={handleHome}
            disabled={homing}
          >
            <Home className="size-3.5" />
            {homing ? "Homing…" : "Home now"}
          </Button>
        </div>
      )}

      <div
        className={cn(
          "flex flex-col gap-6 transition-opacity duration-300 ease-soft lg:flex-row lg:items-center",
          (isPrintActive || !isProfileConfigured) &&
            "pointer-events-none opacity-60"
        )}
      >
        <div className="mx-auto w-full max-w-xs lg:mx-0">
          <BedSchematic
            position={position}
            bedWidthMm={profile.bedWidthMm}
            bedDepthMm={profile.bedDepthMm}
            onJog={jog}
            onHome={handleHome}
            homing={homing}
            homed={homed}
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

      {!isProfileConfigured ? (
        <div className="absolute inset-0 z-10 flex animate-fade flex-col items-center justify-center gap-2 rounded-lg bg-background/40 text-center backdrop-blur-[2px]">
          <Lock className="size-5 animate-pop text-muted-foreground" />
          {/* An auto-detected printer is named here rather than hidden
              behind a generic prompt: the point of the gate is to ask a
              human "is this actually your machine?", and they can't
              answer that without being told what was guessed. */}
          {profile.source === "auto" && profile.printerName ? (
            <p className="max-w-64 text-xs text-muted-foreground">
              This looks like a{" "}
              <span className="font-medium text-foreground">
                {profile.printerName}
              </span>
              . Confirm it in Settings before controlling the printer.
            </p>
          ) : (
            <p className="max-w-56 text-xs text-muted-foreground">
              Choose your printer in Settings before controlling it
            </p>
          )}
          <Button asChild size="sm" variant="secondary">
            <Link to="/settings">
              <SettingsIcon className="size-3.5" />
              {profile.source === "auto" ? "Confirm printer" : "Choose printer"}
            </Link>
          </Button>
        </div>
      ) : (
        isPrintActive && (
          <div className="pointer-events-none absolute inset-0 z-10 flex animate-fade flex-col items-center justify-center gap-1 rounded-lg bg-background/40 text-center backdrop-blur-[2px]">
            <Lock className="size-5 animate-pop text-muted-foreground" />
            <p className="max-w-56 text-xs text-muted-foreground">
              Can&apos;t manually control the printer mid-print
            </p>
          </div>
        )
      )}
    </div>
  );
}
