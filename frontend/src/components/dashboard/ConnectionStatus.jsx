import { Usb, Wifi, WifiOff } from "lucide-react";
import { Badge } from "@/components/ui/badge";

export function ConnectionStatus({ connected, connectionType }) {
  if (!connected) {
    return (
      <Badge variant="destructive" className="w-fit gap-1.5">
        <WifiOff className="size-3" />
        Disconnected
      </Badge>
    );
  }

  const isUsb = connectionType === "usb";

  return (
    <Badge className="w-fit gap-1.5">
      {isUsb ? <Usb className="size-3" /> : <Wifi className="size-3" />}
      Connected · {isUsb ? "USB" : "Wi-Fi"}
    </Badge>
  );
}
