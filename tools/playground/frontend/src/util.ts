/** Escape HTML special characters for safe insertion. */
export function escapeHtml(text: string): string {
  const div = document.createElement("div");
  div.textContent = text;
  return div.innerHTML;
}

/** Normalize a user-entered file name into a clean relative `.dao` path. */
export function normalizeDaoPath(raw: string): string {
  const segments = raw
    .trim()
    .replace(/\\/g, "/")
    .replace(/^\/+|\/+$/g, "")
    .split("/")
    .filter((seg) => seg !== "" && seg !== ".");
  if (segments.length === 0) return "";
  let cleaned = segments.join("/");
  if (!cleaned.endsWith(".dao")) {
    cleaned += ".dao";
  }
  return cleaned;
}

/** Validate a user workspace path against service invariants. */
export function validateUserPath(path: string): string | null {
  if (!path || path === ".dao") {
    return "File name cannot be empty.";
  }
  if (path.includes("..") || path.startsWith("/")) {
    return "Relative file path cannot contain '..'.";
  }
  if (path.startsWith("stdlib/core/") || path.startsWith("stdlib/io/")) {
    return "Paths under stdlib/core/ and stdlib/io/ are reserved for the prelude.";
  }
  const stem = path.slice(0, -4);
  if (!stem || stem.endsWith("/")) {
    return "Invalid .dao file name.";
  }
  return null;
}

/** Derive a valid Dao `module` path from a file path (e.g. `math/vec.dao` -> `math::vec`). */
export function deriveModuleName(path: string): string {
  const withoutExt = path.replace(/\.dao$/, "");
  const segments = withoutExt
    .split("/")
    .filter(Boolean)
    .map((seg) => {
      const sanitized = seg.replace(/[^A-Za-z0-9_]/g, "_");
      return /^[0-9]/.test(sanitized) ? `m_${sanitized}` : sanitized || "mod";
    });
  return segments.length > 0 ? segments.join("::") : "app";
}
