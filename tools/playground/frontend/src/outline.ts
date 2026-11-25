import type { EditorView } from "@codemirror/view";
import type { DocumentSymbol } from "./generated/tooling_surface";
import { jumpInEditor } from "./editor";

/** Render the document symbol tree; clicking an entry jumps to its span. */
export function renderOutline(
  symbols: DocumentSymbol[],
  _view: EditorView,
): void {
  const container = document.getElementById("outline-output")!;
  if (symbols.length === 0) {
    container.replaceChildren(emptyNotice());
    return;
  }
  container.replaceChildren(symbolList(symbols));
}

function emptyNotice(): HTMLElement {
  const el = document.createElement("div");
  el.className = "no-symbols";
  el.textContent = "No symbols";
  return el;
}

function symbolList(symbols: DocumentSymbol[]): HTMLUListElement {
  const list = document.createElement("ul");
  list.className = "outline-list";
  for (const symbol of symbols) {
    const item = document.createElement("li");
    item.className = "outline-item";

    const row = document.createElement("button");
    row.type = "button";
    row.className = "outline-row";
    row.title = `${symbol.kind} ${symbol.name}`;
    row.addEventListener("click", () => {
      jumpInEditor(symbol.offset, symbol.length);
    });

    const kind = document.createElement("span");
    kind.className = "outline-kind";
    kind.textContent = symbol.kind;
    const name = document.createElement("span");
    name.className = "outline-name";
    name.textContent = symbol.name;
    row.append(kind, name);
    item.append(row);

    if (symbol.children.length > 0) {
      item.append(symbolList(symbol.children));
    }
    list.append(item);
  }
  return list;
}
