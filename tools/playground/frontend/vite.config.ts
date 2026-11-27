import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

// The repository's example corpus, which the site imports its snippets from.
const EXAMPLES_DIR = fileURLToPath(new URL("../../../examples", import.meta.url));

export default defineConfig({
  root: ".",
  plugins: [
    {
      // The dev server watches its root and the files it has served.  A file
      // added to the corpus is neither, so the directory is watched too.
      name: "watch-example-corpus",
      configureServer(server) {
        server.watcher.add(EXAMPLES_DIR);
      },
    },
  ],
  server: {
    port: 5173,
    // The one directory outside this root that the dev server may read.
    fs: { allow: [".", EXAMPLES_DIR] },
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
