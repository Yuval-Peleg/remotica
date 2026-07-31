import { useCallback, useState } from "react";
import { extractGcodeThumbnail } from "@/lib/gcode-thumbnail";
import { extractGcodePrintTimeSeconds } from "@/lib/gcode-print-time";

// Thumbnails and the time estimate both live near the top of the file, so
// we only need to read the first slice rather than the whole (potentially
// huge) gcode file.
const HEADER_SLICE_BYTES = 2_000_000;

export function useGcodeFile() {
  const [file, setFile] = useState(null);
  const [thumbnail, setThumbnail] = useState(null);
  const [printTimeSeconds, setPrintTimeSeconds] = useState(null);
  const [error, setError] = useState(null);

  const selectFile = useCallback((selected) => {
    if (!selected) return;

    if (!selected.name.toLowerCase().endsWith(".gcode")) {
      setError("Only .gcode files can be uploaded.");
      return;
    }

    setError(null);
    setFile(selected);
    setThumbnail(null);
    setPrintTimeSeconds(null);

    const reader = new FileReader();
    reader.onload = () => {
      const text = reader.result ?? "";
      setThumbnail(extractGcodeThumbnail(text));
      setPrintTimeSeconds(extractGcodePrintTimeSeconds(text));
    };
    reader.readAsText(selected.slice(0, HEADER_SLICE_BYTES));
  }, []);

  const clearFile = useCallback(() => {
    setFile(null);
    setThumbnail(null);
    setPrintTimeSeconds(null);
    setError(null);
  }, []);

  return { file, thumbnail, printTimeSeconds, error, selectFile, clearFile };
}
