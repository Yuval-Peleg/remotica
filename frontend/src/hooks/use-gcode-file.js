import { useCallback, useState } from "react";
import { api } from "@/lib/api";
import { useFilePreview } from "@/hooks/use-file-preview";
import { extractGcodeThumbnail } from "@/lib/gcode-thumbnail";
import { extractGcodePrintTimeSeconds } from "@/lib/gcode-print-time";

// Thumbnails and the time estimate both live near the top of the file, so
// we only need to read the first slice rather than the whole (potentially
// huge) gcode file.
const HEADER_SLICE_BYTES = 2_000_000;

// Handles picking a local .gcode file: validates the extension, parses its
// embedded thumbnail/print-time client-side for instant feedback, and
// uploads it to the backend so it's actually saved on the PC running this
// app (see POST /api/upload in api_handlers.c). The queued file's name,
// status, and progress all come from the backend's live state instead
// (usePrinterState) — this hook is only responsible for the "picking and
// uploading" step, not for tracking what's currently selected.
export function useGcodeFile() {
  const [error, setError] = useState(null);
  const [uploading, setUploading] = useState(false);

  const selectFile = useCallback((selected) => {
    if (!selected) return;

    if (!selected.name.toLowerCase().endsWith(".gcode")) {
      setError("Only .gcode files can be uploaded.");
      return;
    }

    setError(null);

    const reader = new FileReader();
    reader.onload = () => {
      const text = reader.result ?? "";
      useFilePreview.cache(selected.name, {
        thumbnail: extractGcodeThumbnail(text),
        printTimeSeconds: extractGcodePrintTimeSeconds(text),
      });
    };
    reader.readAsText(selected.slice(0, HEADER_SLICE_BYTES));

    setUploading(true);
    api
      .uploadFile(selected)
      .catch(() => setError("Couldn't upload the file to the backend."))
      .finally(() => setUploading(false));
  }, []);

  return { error, uploading, selectFile };
}
