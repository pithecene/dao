import { createServer } from "vite";
import geomSource from "../../../../examples/geometry/geom.dao?raw";

// The dev server may read the frontend and the example corpus, and nothing
// else in the repository.  This starts it with the project's own config and
// asks for one file on each side of that boundary.

async function statusAndBody(url: string): Promise<{ status: number; body: string }> {
  const reply = await fetch(url);
  return { status: reply.status, body: await reply.text() };
}

async function run(): Promise<void> {
  const server = await createServer({ configFile: "vite.config.ts", logLevel: "silent" });
  await server.listen();
  try {
    const base = server.resolvedUrls?.local[0];
    if (!base) throw new Error("the dev server reported no local address");
    // The server's root is this frontend, three directories below the
    // repository's.  The path is cut as a path, never parsed as a URL.
    const repoRoot = server.config.root.split("/").slice(0, -3).join("/");
    // The server decodes a request's path with `decodeURI`, the inverse of
    // the `encodeURI` applied here.  A checkout whose path holds `#` or `?`
    // never reaches this point: Vite cuts such a path short when it resolves
    // a file, so it cannot bundle this suite there.
    // The route is `/@fs/` followed by the path without its leading slash,
    // which a drive-letter path does not have.
    const inRepo = (path: string): string =>
      `${base}@fs/${encodeURI(`${repoRoot.replace(/^\//, "")}/${path}`)}?raw`;

    const example = await statusAndBody(inRepo("examples/geometry/geom.dao"));
    if (example.status !== 200 || example.body !== geomSource) {
      throw new Error(`an imported example must be served with its text, got ${example.status}`);
    }

    const outside = await statusAndBody(inRepo("AGENTS.md"));
    if (outside.status !== 403) {
      throw new Error(`a repository file outside the corpus must be refused, got ${outside.status}`);
    }
    console.log("dev_server_access.test: all assertions passed");
  } finally {
    await server.close();
  }
}

void run().catch((err: unknown) => {
  console.error(err);
  throw err;
});
