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
