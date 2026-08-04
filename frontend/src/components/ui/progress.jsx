"use client";

import * as React from "react";
import { Progress as ProgressPrimitive } from "radix-ui";

import { cn } from "@/lib/utils";

// `active` adds a highlight travelling along the filled portion, for
// progress that is genuinely still moving. It's the only thing on the
// dashboard that distinguishes "printing" from "paused at 40%" at a
// glance, so it's deliberately opt-in rather than always on.
function Progress({ className, value, active = false, ...props }) {
  return (
    <ProgressPrimitive.Root
      data-slot="progress"
      className={cn(
        "relative flex h-1 w-full items-center overflow-x-hidden rounded-full bg-muted",
        className
      )}
      {...props}
    >
      <ProgressPrimitive.Indicator
        data-slot="progress-indicator"
        className="relative size-full flex-1 overflow-hidden bg-primary transition-transform duration-500 ease-soft"
        style={{ transform: `translateX(-${100 - (value || 0)}%)` }}
      >
        {active && (
          <span className="absolute inset-0 animate-sheen bg-gradient-to-r from-transparent via-primary-foreground/40 to-transparent" />
        )}
      </ProgressPrimitive.Indicator>
    </ProgressPrimitive.Root>
  );
}

export { Progress };
