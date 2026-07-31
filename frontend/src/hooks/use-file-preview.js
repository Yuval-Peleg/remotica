import { useEffect, useState } from "react";
import { api } from "@/lib/api";
import { extractGcodeThumbnail } from "@/lib/gcode-thumbnail";
import { extractGcodePrintTimeSeconds } from "@/lib/gcode-print-time";

// Thumbnails and print-time estimates are parsed once per filename and
// kept here for the lifetime of the page — gcode files on the backend are
// immutable once uploaded (re-uploading the same name just overwrites it,
// which is rare enough not to worry about a stale cache entry here).
const previewCache = new Map();

function parsePreview(gcodeText) {
  return {
    thumbnail: extractGcodeThumbnail(gcodeText),
    printTimeSeconds: extractGcodePrintTimeSeconds(gcodeText),
  };
}

// Gives back {thumbnail, printTimeSeconds} for a filename that's stored on
// the backend. If nothing's cached yet, this fetches the file's raw
// content and parses it client-side — reusing the same parsers used for a
// freshly-selected local file, instead of duplicating gcode-parsing logic
// in the backend (see the comment on GET /api/files/content in
// api_handlers.c for why).
export function useFilePreview(filename) {
  const [preview, setPreview] = useState(() =>
    filename ? (previewCache.get(filename) ?? null) : null
  );

  useEffect(() => {
    if (!filename) {
      setPreview(null);
      return;
    }

    const cached = previewCache.get(filename);
    if (cached) {
      setPreview(cached);
      return;
    }

    let cancelled = false;
    api
      .getFileContent(filename)
      .then((text) => {
        if (cancelled) return;
        const result = parsePreview(text);
        previewCache.set(filename, result);
        setPreview(result);
      })
      .catch(() => {
        if (!cancelled) setPreview({ thumbnail: null, printTimeSeconds: null });
      });

    return () => {
      cancelled = true;
    };
  }, [filename]);

  return preview ?? { thumbnail: null, printTimeSeconds: null };
}

// Lets a caller (useGcodeFile, right after a local file is selected)
// populate the cache immediately from a File it already parsed, so
// useFilePreview doesn't re-fetch from the backend something we just read
// off disk a moment ago.
useFilePreview.cache = (filename, preview) => {
  previewCache.set(filename, preview);
};
