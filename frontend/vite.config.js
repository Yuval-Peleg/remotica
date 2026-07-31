import path from "path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

// https://vite.dev/config/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@": path.resolve(import.meta.dirname, "./src"),
    },
  },
  server: {
    // Forward API calls to the C backend during development. In
    // production the backend serves the built frontend itself, so
    // requests to /api are already same-origin and this isn't needed.
    proxy: {
      "/api": "http://localhost:8080",
    },
  },
});
