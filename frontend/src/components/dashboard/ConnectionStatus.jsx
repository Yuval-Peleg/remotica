import { Usb, Wifi, WifiOff } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { parsePrinterName } from "@/lib/parse-printer-name";

// Both branches return a Badge in the same position, so React would
// normally reuse the element and swap its contents. The differing keys
// force a remount instead, which is what replays animate-pop — losing a
// printer should register as something that just happened, not as text
// that quietly changed while you weren't looking.
export function ConnectionStatus({ connected, connectionType, firmwareInfo }) {
  if (!connected) {
    return (
      <Badge
        key="disconnected"
        variant="destructive"
        className="w-fit animate-pop gap-1.5"
      >
        <WifiOff className="size-3" />
        Disconnected
      </Badge>
    );
  }

  const isUsb = connectionType === "usb";
  const printerName = parsePrinterName(firmwareInfo);

  return (
    <Badge
      key="connected"
      className="w-fit animate-pop gap-1.5 overflow-visible"
    >
      {/* Badge is overflow-hidden by default, which would clip the halo
          as it expands past the dot — hence overflow-visible above. */}
      <span className="relative flex size-1.5 shrink-0">
        <span className="absolute inset-0 animate-halo rounded-full bg-primary-foreground" />
        <span className="relative size-1.5 rounded-full bg-primary-foreground" />
      </span>
      {isUsb ? <Usb className="size-3" /> : <Wifi className="size-3" />}
      Connected · {isUsb ? "USB" : "Wi-Fi"}
      {printerName && <span className="opacity-80">· {printerName}</span>}
    </Badge>
  );
}
