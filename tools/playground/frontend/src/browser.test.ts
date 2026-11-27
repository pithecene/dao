import { chromium, type Page } from "playwright";

// The playground in a real browser, against a running compiler service.
// These are the behaviours that no test without a page can hold: where
// focus goes, what the editor draws, and what a key does.
//
// The playground's address is fixed when this suite is built, from
// VITE_PLAYGROUND_URL.  `tools/playground/browser_test.sh` starts the
// service, builds this suite against it, and runs it.

const PLAYGROUND_URL: string | undefined = import.meta.env.VITE_PLAYGROUND_URL;
const WAIT_MS = 10_000;
const POLL_MS = 50;

/** What the page shows of the editor at one moment. */
interface EditorProbe {
  theme: string | undefined;
  focused: boolean;
  outlined: boolean;
  lines: string[];
  /** Each classified span: its token class and the text it covers. */
  tokens: [string, string][];
  tabTitles: string[];
  activeTab: number;
  /** Whether the instruction for leaving the editor is drawn, and under the editor, not over it. */
  exitHintUnderEditor: boolean;
}

function probe(page: Page): Promise<EditorProbe> {
  return page.evaluate(() => {
    const outline = getComputedStyle(document.querySelector(".cm-editor")!);
    const tabs = [...document.querySelectorAll(".editor-tab")];
    const hint = document.querySelector("#editor-exit-hint")!.getBoundingClientRect();
    const editor = document.querySelector("#editor-container")!.getBoundingClientRect();
    return {
      theme: document.documentElement.dataset.theme,
      focused: document.activeElement?.classList.contains("cm-content") ?? false,
      outlined: outline.outlineStyle !== "none" && parseFloat(outline.outlineWidth) > 0,
      lines: [...document.querySelectorAll(".cm-content .cm-line")].map(
        (line) => line.textContent ?? "",
      ),
      tokens: [...document.querySelectorAll(".cm-content [class*='dao-']")].map(
        (span): [string, string] => [
          [...span.classList].find((name) => name.startsWith("dao-")) ?? "",
          span.textContent ?? "",
        ],
      ),
      tabTitles: tabs.map((tab) => tab.textContent?.trim() ?? ""),
      activeTab: tabs.findIndex((tab) => tab.classList.contains("active")),
      // Half a pixel of slack: the two boxes share an edge at a fractional offset.
      exitHintUnderEditor: hint.height > 0 && hint.top >= editor.bottom - 0.5,
    };
  });
}

/** Reads the page until the claim holds, and names the claim when it never does. */
async function eventually(
  page: Page,
  claim: string,
  holds: (editor: EditorProbe) => boolean,
): Promise<EditorProbe> {
  let last = await probe(page);
  for (let waited = 0; !holds(last); waited += POLL_MS) {
    if (waited >= WAIT_MS) throw new Error(`never held: ${claim}\n  ${JSON.stringify(last)}`);
    await page.waitForTimeout(POLL_MS);
    last = await probe(page);
  }
  return last;
}

async function replaceEditorText(page: Page, text: string): Promise<void> {
  await page.click(".cm-content");
  await page.keyboard.press("ControlOrMeta+A");
  await page.keyboard.press("Backspace");
  await page.keyboard.insertText(text);
}

/** Moves focus out of the editor, where Tab alone indents. */
async function leaveEditor(page: Page): Promise<void> {
  await page.keyboard.press("Escape");
  await page.keyboard.press("Tab");
  await eventually(page, "Escape and then Tab leaves the editor", (editor) => !editor.focused);
}

async function focusIsVisibleInBothThemes(page: Page): Promise<void> {
  const MOST_STOPS_BEFORE_EDITOR = 40;
  const first = (await probe(page)).theme;
  for (const toggled of [false, true]) {
    if (toggled) {
      await page.click("#theme-toggle");
      await eventually(page, "the toggle changes the theme", (editor) => editor.theme !== first);
    }
    const theme = (await probe(page)).theme;
    await eventually(page, `${theme}: no outline without focus`, (editor) => !editor.outlined);
    for (let stops = 0; !(await probe(page)).focused; stops++) {
      if (stops >= MOST_STOPS_BEFORE_EDITOR) throw new Error(`${theme}: Tab never reaches the editor`);
      await page.keyboard.press("Tab");
    }
    await eventually(page, `${theme}: the focused editor is outlined`, (editor) => editor.outlined);
    await leaveEditor(page);
    await eventually(page, `${theme}: the outline clears with focus`, (editor) => !editor.outlined);
  }
}

async function editorIsNamedOnEveryFile(page: Page): Promise<void> {
  await page.selectOption("#example-select", "modules");
  const loaded = await eventually(
    page,
    "the modules example opens three files",
    (editor) => editor.tabTitles.length === 3,
  );
  for (const [index, title] of loaded.tabTitles.entries()) {
    await page.locator(".editor-tab").nth(index).click();
    await eventually(page, `${title} becomes the open file`, (editor) => editor.activeTab === index);
    const content = page.locator(".cm-content");
    if ((await content.getAttribute("aria-label")) !== "Source editor") {
      throw new Error(`the editor has no name on ${title}`);
    }
    if (!(await content.getAttribute("aria-description"))?.includes("Escape")) {
      throw new Error(`the editor does not say how to leave it on ${title}`);
    }
  }
}

async function tokensCoverTheirOwnText(page: Page): Promise<void> {
  // The quotes and the emoji each take more bytes than code units, so a
  // span counted in bytes would land to the right of its text from there on.
  const text = "\"“hi” \u{1F600}\"";
  await replaceEditorText(page, `fn main(): i32\n  print(${text})\n  return 0\n`);
  const expected: [string, string][] = [
    ["dao-literal-string", text],
    ["dao-keyword", "return"],
    ["dao-literal-number", "0"],
  ];
  const drawn = (editor: EditorProbe, [kind, lexeme]: [string, string]): boolean =>
    editor.tokens.some(([drawnKind, drawnText]) => drawnKind === kind && drawnText === lexeme);
  await eventually(page, "each token after a wide character covers its own text", (editor) =>
    expected.every((token) => drawn(editor, token)),
  );
}

async function tabIndentsAndEscapeLeaves(page: Page): Promise<void> {
  const flat = ["fn main(): i32", "return 0", "let x = 1", ""];
  // Two spaces a level: a tab character is not legal in Dao source.
  const indented = ["fn main(): i32", "  return 0", "  let x = 1", ""];
  const shows = (editor: EditorProbe, lines: string[]): boolean =>
    editor.lines.join("\n") === lines.join("\n");

  await replaceEditorText(page, flat.join("\n"));
  await eventually(page, "the text is in the editor", (editor) => shows(editor, flat));
  await eventually(page, "the way out is stated under the editor", (editor) =>
    editor.exitHintUnderEditor,
  );

  // Select from the second line's start to the third line's end.
  await page.keyboard.press("ControlOrMeta+Home");
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("Shift+ArrowDown");
  await page.keyboard.press("Shift+End");
  await page.keyboard.press("Tab");
  await eventually(
    page,
    "Tab indents every selected line and keeps focus",
    (editor) => editor.focused && shows(editor, indented),
  );
  await page.keyboard.press("Shift+Tab");
  await eventually(
    page,
    "Shift+Tab removes the indentation and keeps focus",
    (editor) => editor.focused && shows(editor, flat),
  );

  // The selection is still held, and a binding takes Escape to drop it.
  await leaveEditor(page);
  await eventually(
    page,
    "leaving changes no text",
    (editor) => shows(editor, flat),
  );
}

async function tabPassesThroughReadOnlyFile(page: Page): Promise<void> {
  await page.click(".sidebar-header.collapsible-header");
  await page.locator("#prelude-list li").first().click();
  await eventually(page, "a prelude file opens", (editor) => editor.tabTitles.length === 2);
  await page.click(".cm-content");
  const opened = await eventually(page, "the read-only editor takes focus", (editor) =>
    editor.focused,
  );
  await page.keyboard.press("Tab");
  await eventually(
    page,
    "Tab moves focus on from a read-only file and changes nothing",
    (editor) => !editor.focused && editor.lines.join("\n") === opened.lines.join("\n"),
  );
}

async function run(): Promise<void> {
  if (!PLAYGROUND_URL) throw new Error("VITE_PLAYGROUND_URL must name a running playground");
  const scenarios = [
    focusIsVisibleInBothThemes,
    editorIsNamedOnEveryFile,
    tokensCoverTheirOwnText,
    tabIndentsAndEscapeLeaves,
    tabPassesThroughReadOnlyFile,
  ];
  const browser = await chromium.launch();
  try {
    for (const scenario of scenarios) {
      // A page of its own, so that no scenario reads another's saved draft.
      const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
      await page.goto(`${PLAYGROUND_URL}#playground`, { waitUntil: "networkidle" });
      await scenario(page);
      await page.close();
    }
  } finally {
    await browser.close();
  }
  console.log("browser.test: all assertions passed");
}

void run().catch((err: unknown) => {
  console.error(err);
  throw err;
});
