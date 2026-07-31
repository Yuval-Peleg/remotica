// Marlin's M115 reply (see backend's PrinterState.firmwareInfo) usually
// includes a MACHINE_TYPE:<value> field among a bunch of other stuff
// nobody wants to read in a UI. Pulls just that value out for display —
// still just a best-effort hint (see Settings.jsx's full caveat), not
// reliable identification.
export function parsePrinterName(firmwareInfo) {
  if (!firmwareInfo) return null;

  const match = firmwareInfo.match(/\bMACHINE_TYPE:(\S+)/);
  if (!match) return null;

  // Some firmwares (e.g. Prusa's) use underscores in place of spaces in
  // this field, since the surrounding reply is space-delimited — turn
  // those back into spaces for display. A no-op for values with no
  // underscores in the first place.
  return match[1].replace(/_/g, " ");
}
