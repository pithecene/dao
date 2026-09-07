import {
  ROUTES,
  type RouteName,
  type RouteTypes,
} from "./generated/tooling_surface";

// Typed client over the compiler service.  Routes, request bodies, and
// response types all come from the generated tooling surface, so a route
// the service adds is unusable here until the surface is regenerated —
// and a route the frontend calls must exist in the surface.

/** How long a transport failure is retried before it is reported. */
const RETRY_WINDOW_MS = 8000;
const RETRY_DELAY_MS = 400;

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

/** A reply that means the service is not there right now, not a bad request. */
class TransientError extends Error {}

type StatusListener = (restarting: boolean) => void;
const statusListeners: StatusListener[] = [];
let restarting = false;

/** Be told when the service stops answering (a rebuild restarts it) and when it is back. */
export function onServiceStatus(listener: StatusListener): void {
  statusListeners.push(listener);
}

function setRestarting(value: boolean): void {
  if (value === restarting) return;
  restarting = value;
  for (const listener of statusListeners) listener(value);
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Substitute `:param` segments from the request's fields. */
function resolvePath(path: string, request: unknown): string {
  const fields = (request ?? {}) as Record<string, unknown>;
  return path.replace(/:(\w+)/g, (_match, key: string) =>
    encodeURIComponent(String(fields[key])),
  );
}

/**
 * Call a route.  Transport failures and gateway errors — what the dev
 * proxy reports while the compiler service restarts after a rebuild —
 * are retried for a few seconds before surfacing.
 */
export async function api<R extends RouteName>(
  route: R,
  request: RouteTypes[R]["request"],
): Promise<RouteTypes[R]["response"]> {
  const spec = ROUTES[route];
  const path = resolvePath(spec.path, request);
  const init: RequestInit =
    spec.method === "POST"
      ? {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(request),
        }
      : { method: "GET" };

  const deadline = Date.now() + RETRY_WINDOW_MS;
  for (;;) {
    try {
      const resp = await fetch(path, init);
      if (resp.status >= 500) throw new TransientError(`${resp.status}`);
      setRestarting(false);
      if (!resp.ok) {
        const body = (await resp.json().catch(() => ({}))) as {
          error?: string;
        };
        throw new ApiError(resp.status, body.error ?? resp.statusText);
      }
      return (await resp.json()) as RouteTypes[R]["response"];
    } catch (err) {
      // fetch rejects with a TypeError when the connection itself fails.
      const transient =
        err instanceof TransientError || err instanceof TypeError;
      if (!transient || Date.now() > deadline) {
        setRestarting(false);
        throw err;
      }
      setRestarting(true);
      await sleep(RETRY_DELAY_MS);
    }
  }
}
