// Thin wrapper around the backend's REST API (see backend/src/api_handlers.c
// and the root CLAUDE.md for the full route list). Every function here
// returns a Promise that resolves to the parsed JSON response, or rejects
// with an Error carrying the backend's error message if the request failed.

async function request(path, options) {
  const res = await fetch(path, options);
  if (!res.ok) {
    const message = await res.text().catch(() => "");
    throw new Error(message || `${res.status} ${res.statusText}`);
  }
  return res;
}

async function requestJson(path, options) {
  const res = await request(path, options);
  return res.json();
}

function postJson(path, body) {
  return requestJson(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

function post(path) {
  return requestJson(path, { method: "POST" });
}

export const api = {
  getState: () => requestJson("/api/state"),

  jog: (axis, deltaMm) => postJson("/api/jog", { axis, deltaMm }),
  home: () => post("/api/home"),
  setTemp: (heater, celsius) => postJson("/api/temp", { heater, celsius }),

  getProfile: () => requestJson("/api/profile"),
  setProfile: (partialProfile) => postJson("/api/profile", partialProfile),

  getPrinterDatabase: async () => {
    const { printers } = await requestJson("/api/printer-database");
    return printers;
  },

  // What the attached printer's firmware reply suggests it is, computed
  // fresh on each call — independent of what's actually saved in the
  // profile. Resolves to { connected, firmwareInfo, match } where match is
  // a printer-database entry or null.
  getPrinterSuggestion: () => requestJson("/api/printer-suggestion"),

  // Host-level status rather than printer status — version, uptime, disk,
  // and whether this is an installed systemd service. `managed` is false
  // in a dev checkout, where there's no service to change; setBootStart
  // 409s in that case rather than pretending to work.
  // Start/end G-code that runs around every print. Its own endpoint
  // rather than part of the profile — see backend/src/gcode_snippets.h.
  getSnippets: () => requestJson("/api/gcode-snippets"),
  setSnippets: (snippets) => postJson("/api/gcode-snippets", snippets),

  getSystem: () => requestJson("/api/system"),
  setBootStart: (enabled) => postJson("/api/system/boot-start", { enabled }),

  uploadFile: (file) =>
    requestJson(`/api/upload?filename=${encodeURIComponent(file.name)}`, {
      method: "POST",
      body: file,
    }),

  printStart: () => post("/api/print/start"),
  printPause: () => post("/api/print/pause"),
  printResume: () => post("/api/print/resume"),
  printCancel: () => post("/api/print/cancel"),

  getConsoleBacklog: async () => {
    const { entries } = await requestJson("/api/console");
    return entries;
  },

  listFiles: () => requestJson("/api/files"),
  getFileContent: async (filename) => {
    const res = await request(
      `/api/files/content?filename=${encodeURIComponent(filename)}`
    );
    return res.text();
  },
  selectFile: (filename) =>
    post(`/api/files/select?filename=${encodeURIComponent(filename)}`),
  deleteFile: (filename) =>
    post(`/api/files/delete?filename=${encodeURIComponent(filename)}`),

  getCameraInfo: () => requestJson("/api/camera"),
};
