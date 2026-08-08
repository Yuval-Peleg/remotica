import { useEffect, useState } from "react";
import { ChevronRight, FileCode, Trash2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { api } from "@/lib/api";
import { useFilePreview } from "@/hooks/use-file-preview";
import { formatDurationShort } from "@/lib/gcode-print-time";
import { cn } from "@/lib/utils";

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

// modifiedAt is a Unix timestamp in seconds (backend's st_mtime), Date
// wants milliseconds.
function formatDate(modifiedAtSeconds) {
  return new Date(modifiedAtSeconds * 1000).toLocaleDateString([], {
    month: "short",
    day: "numeric",
  });
}

// One row in the list. Its own component (rather than inlined in the map()
// below) because useFilePreview is a hook — it has to be called once per
// file, which means once per component instance, not once per render of a
// parent that loops over an array.
function FileRow({ file, isSelected, disabled, onSelect, onDelete }) {
  const { thumbnail, printTimeSeconds } = useFilePreview(file.filename);

  return (
    <div
      className={cn(
        "flex items-center gap-3 rounded-lg border p-2 transition-colors duration-200 ease-soft",
        isSelected
          ? "border-primary bg-primary/10"
          : "border-border bg-secondary/20 hover:border-border/80 hover:bg-secondary/40"
      )}
    >
      <button
        type="button"
        onClick={onSelect}
        disabled={disabled}
        className="flex min-w-0 flex-1 items-center gap-3 text-left disabled:cursor-not-allowed disabled:opacity-50"
      >
        <div className="flex size-10 shrink-0 items-center justify-center overflow-hidden rounded-md border border-border bg-secondary/40">
          {thumbnail ? (
            <img
              src={thumbnail}
              alt=""
              className="h-full w-full object-cover"
            />
          ) : (
            <FileCode className="size-4 text-muted-foreground" />
          )}
        </div>
        <div className="min-w-0 flex-1">
          <p
            className="truncate text-sm font-medium text-foreground"
            title={file.filename}
          >
            {file.filename}
          </p>
          <p className="text-xs text-muted-foreground">
            {formatBytes(file.size)}
            {printTimeSeconds != null
              ? ` · ${formatDurationShort(printTimeSeconds)}`
              : ""}
            {` · ${formatDate(file.modifiedAt)}`}
          </p>
        </div>
      </button>

      <Button
        type="button"
        variant="ghost"
        size="icon-sm"
        disabled={disabled}
        onClick={onDelete}
        className="shrink-0 text-muted-foreground hover:text-destructive"
      >
        <Trash2 className="size-4" />
        <span className="sr-only">Delete {file.filename}</span>
      </Button>
    </div>
  );
}

// Collapsible "On device" section: lists every gcode file stored on this
// PC (see GET /api/files), lets you pick one to queue for printing again
// without re-uploading it, or delete it. `refreshTrigger` is any value
// that changes when the list might be stale (e.g. the currently queued
// filename) — passing it in just re-fetches, it doesn't need to mean
// anything on its own.
export function OnDeviceFiles({
  currentFilename,
  printActive,
  refreshTrigger,
}) {
  const [expanded, setExpanded] = useState(false);
  const [files, setFiles] = useState([]);

  const loadFiles = () => {
    api
      .listFiles()
      .then((res) => {
        const sorted = [...res.files].sort(
          (a, b) => b.modifiedAt - a.modifiedAt
        );
        setFiles(sorted);
      })
      .catch(() => {});
  };

  useEffect(() => {
    if (expanded) loadFiles();
    // Only re-fetch while the section is actually open — no point polling
    // a list nobody can see.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [expanded, refreshTrigger]);

  return (
    <div>
      <button
        type="button"
        onClick={() => setExpanded((value) => !value)}
        className="flex w-full items-center justify-between py-1.5 text-sm text-muted-foreground transition-colors hover:text-foreground"
      >
        <span>On device</span>
        <ChevronRight
          className={cn(
            "size-4 transition-transform duration-300 ease-spring",
            expanded && "rotate-90"
          )}
        />
      </button>

      <div
        className={cn(
          "grid transition-[grid-template-rows] duration-400 ease-smooth",
          expanded ? "grid-rows-[1fr]" : "grid-rows-[0fr]"
        )}
      >
        <div className="overflow-hidden">
          <div className="flex motion-stagger flex-col gap-2 pt-1 pb-1">
            {files.length === 0 ? (
              <p className="py-2 text-center text-xs text-muted-foreground">
                No gcode files saved on this device yet.
              </p>
            ) : (
              files.map((file) => (
                <FileRow
                  key={file.filename}
                  file={file}
                  isSelected={file.filename === currentFilename}
                  disabled={printActive}
                  onSelect={() => api.selectFile(file.filename).catch(() => {})}
                  onDelete={() =>
                    api
                      .deleteFile(file.filename)
                      .then(loadFiles)
                      .catch(() => {})
                  }
                />
              ))
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
