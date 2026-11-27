import { defineConfig } from "vite";

export default defineConfig({
  root: ".",
  server: {
    port: 5173,
    // The site imports its snippets from the repository's `examples/`, the
    // one directory outside this root that the dev server may read.
    fs: { allow: [".", "../../../examples"] },
    proxy: {
      "/api": {
        target: "http://127.0.0.1:8090",
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
  },
});
