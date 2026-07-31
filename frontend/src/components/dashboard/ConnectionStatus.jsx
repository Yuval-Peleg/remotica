import { Usb, Wifi, WifiOff } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { parsePrinterName } from "@/lib/parse-printer-name";

export function ConnectionStatus({ connected, connectionType, firmwareInfo }) {
  if (!connected) {
    return (
      <Badge variant="destructive" className="w-fit gap-1.5">
        <WifiOff className="size-3" />
        Disconnected
      </Badge>
    );
  }

  const isUsb = connectionType === "usb";
  const printerName = parsePrinterName(firmwareInfo);

  return (
    <Badge className="w-fit gap-1.5">
      {isUsb ? <Usb className="size-3" /> : <Wifi className="size-3" />}
      Connected · {isUsb ? "USB" : "Wi-Fi"}
      {printerName && <span className="opacity-80">· {printerName}</span>}
    </Badge>
  );
}
