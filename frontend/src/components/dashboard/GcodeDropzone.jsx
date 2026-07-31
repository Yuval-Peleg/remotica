import { useState } from "react";
import { FileCode, UploadCloud, X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { cn } from "@/lib/utils";

export function GcodeDropzone({
  filename,
  thumbnail,
  error,
  locked,
  onSelect,
  onClear,
}) {
  const [isDragOver, setIsDragOver] = useState(false);

  const handleDrop = (event) => {
    event.preventDefault();
    setIsDragOver(false);
    const dropped = event.dataTransfer.files?.[0];
    if (dropped) onSelect(dropped);
  };

  if (filename) {
    return (
      <div className="relative aspect-square w-full overflow-hidden rounded-lg border border-border bg-secondary/40">
        {thumbnail ? (
          <img
            src={thumbnail}
            alt={`${filename} thumbnail`}
            className="h-full w-full object-cover"
          />
        ) : (
          <div className="flex h-full w-full flex-col items-center justify-center gap-2 text-muted-foreground">
            <FileCode className="size-10" />
            <span className="text-xs">No preview available</span>
          </div>
        )}
        {!locked && (
          <Button
            type="button"
            variant="secondary"
            size="icon"
            className="absolute top-2 right-2 size-7"
            onClick={onClear}
          >
            <X className="size-4" />
            <span className="sr-only">Remove file</span>
          </Button>
        )}
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-2">
      <label
        onDragOver={(event) => {
          event.preventDefault();
          setIsDragOver(true);
        }}
        onDragLeave={() => setIsDragOver(false)}
        onDrop={handleDrop}
        className={cn(
          "flex aspect-square w-full cursor-pointer flex-col items-center justify-center gap-2 rounded-lg border-2 border-dashed border-border text-center transition-colors hover:border-primary hover:bg-secondary/30",
          isDragOver && "border-primary bg-secondary/30"
        )}
      >
        <UploadCloud className="size-8 text-muted-foreground" />
        <span className="text-sm font-medium text-foreground">
          Drop a .gcode file here
        </span>
        <span className="text-xs text-muted-foreground">
          or click to browse
        </span>
        <input
          type="file"
          accept=".gcode"
          className="hidden"
          onChange={(event) => {
            const selected = event.target.files?.[0];
            if (selected) onSelect(selected);
            event.target.value = "";
          }}
        />
      </label>

      {error && (
        <Alert variant="destructive">
          <AlertDescription>{error}</AlertDescription>
        </Alert>
      )}
    </div>
  );
}
