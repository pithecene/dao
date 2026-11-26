const LAYOUT_STORAGE_KEY = "dao.workspace.layout.v1";

interface SavedLayout {
  sidebarPx?: number;
  rightPct?: number;
  consolePct?: number;
}

function readLayout(): SavedLayout {
  try {
    const raw = localStorage.getItem(LAYOUT_STORAGE_KEY);
    return raw ? (JSON.parse(raw) as SavedLayout) : {};
  } catch {
    return {};
  }
}

function writeLayout(patch: Partial<SavedLayout>): void {
  try {
    const next = { ...readLayout(), ...patch };
    localStorage.setItem(LAYOUT_STORAGE_KEY, JSON.stringify(next));
  } catch {
    // Ignore storage quota errors.
  }
}

export function initSplitters(): void {
  const main = document.getElementById("workspace-main")!;
  const rightPanels = document.getElementById("right-panels")!;
  const sidebarSplitter = document.getElementById("splitter-sidebar")!;
  const rightSplitter = document.getElementById("splitter-right")!;
  const consoleSplitter = document.getElementById("splitter-console")!;

  const saved = readLayout();
  if (saved.sidebarPx) {
    main.style.setProperty("--sidebar-width", `${saved.sidebarPx}px`);
  }
  if (saved.rightPct) {
    main.style.setProperty("--right-width", `${saved.rightPct}%`);
  }
  if (saved.consolePct) {
    rightPanels.style.setProperty("--console-height", `${saved.consolePct}%`);
  }

  bindPointerDrag(sidebarSplitter, (e) => {
    const rect = main.getBoundingClientRect();
    const px = Math.round(
      Math.max(160, Math.min(400, e.clientX - rect.left)),
    );
    main.style.setProperty("--sidebar-width", `${px}px`);
    writeLayout({ sidebarPx: px });
  });

  bindPointerDrag(rightSplitter, (e) => {
    const rect = main.getBoundingClientRect();
    if (rect.width <= 0) return;
    const rightPx = rect.right - e.clientX;
    const pct = Math.max(
      24,
      Math.min(66, Math.round((rightPx / rect.width) * 1000) / 10),
    );
    main.style.setProperty("--right-width", `${pct}%`);
    writeLayout({ rightPct: pct });
  });

  bindPointerDrag(consoleSplitter, (e) => {
    const rect = rightPanels.getBoundingClientRect();
    if (rect.height <= 0) return;
    const topPx = e.clientY - rect.top;
    const pct = Math.max(
      18,
      Math.min(72, Math.round((topPx / rect.height) * 1000) / 10),
    );
    rightPanels.style.setProperty("--console-height", `${pct}%`);
    writeLayout({ consolePct: pct });
  });

  // Keyboard support for accessible separator adjustment.
  sidebarSplitter.addEventListener("keydown", (e) => {
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    e.preventDefault();
    const cur = parseFloat(getComputedStyle(main).getPropertyValue("--sidebar-width")) || 224;
    const next = Math.max(160, Math.min(400, cur + (e.key === "ArrowRight" ? 16 : -16)));
    main.style.setProperty("--sidebar-width", `${next}px`);
    writeLayout({ sidebarPx: next });
  });

  rightSplitter.addEventListener("keydown", (e) => {
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    e.preventDefault();
    const cur = parseFloat(main.style.getPropertyValue("--right-width")) || 44;
    const next = Math.max(24, Math.min(66, cur + (e.key === "ArrowLeft" ? 3 : -3)));
    main.style.setProperty("--right-width", `${next}%`);
    writeLayout({ rightPct: next });
  });

  consoleSplitter.addEventListener("keydown", (e) => {
    if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
    e.preventDefault();
    const cur = parseFloat(rightPanels.style.getPropertyValue("--console-height")) || 36;
    const next = Math.max(18, Math.min(72, cur + (e.key === "ArrowDown" ? 4 : -4)));
    rightPanels.style.setProperty("--console-height", `${next}%`);
    writeLayout({ consolePct: next });
  });
}

function bindPointerDrag(
  handle: HTMLElement,
  onMove: (e: PointerEvent) => void,
): void {
  handle.addEventListener("pointerdown", (startEvent: PointerEvent) => {
    startEvent.preventDefault();
    handle.setPointerCapture(startEvent.pointerId);
    handle.classList.add("dragging");

    const move = (moveEvent: PointerEvent) => onMove(moveEvent);
    const stop = (endEvent: PointerEvent) => {
      handle.classList.remove("dragging");
      if (handle.hasPointerCapture(endEvent.pointerId)) {
        handle.releasePointerCapture(endEvent.pointerId);
      }
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", stop);
      handle.removeEventListener("pointercancel", stop);
    };

    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", stop);
    handle.addEventListener("pointercancel", stop);
  });
}
