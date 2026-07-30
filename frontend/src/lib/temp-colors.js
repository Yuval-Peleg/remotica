// Shared between the temperature graph and the control panel's temp dials so
// a given axis (hotend/bed) always reads as the same color everywhere.
// Categorical dark-mode slots from the design system's validated palette,
// checked against Remotica's dark surface (#1b1c1a) — both pairs pass CVD
// separation (worst adjacent ΔE 26.8 protan / 32.4 tritan) and contrast.
export const TEMP_COLORS = {
  hotend: "#d95926", // categorical slot 2 (orange) — matches the common "hotend = warm" convention
  bed: "#3987e5", // categorical slot 1 (blue)
};
