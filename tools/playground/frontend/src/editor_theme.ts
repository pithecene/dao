/**
 * The Pithecene CodeMirror theme's rules, by selector.  Every colour is a
 * token, so the rules adapt to the dark and light themes.  They are data
 * apart from the editor so that a test can read what the editor draws.
 */
export const editorThemeRules: Record<string, Record<string, string>> = {
  "&": {
    backgroundColor: "var(--editor-bg)",
    color: "var(--fg-primary)",
    height: "100%",
  },
  ".cm-content": {
    caretColor: "var(--editor-cursor)",
    padding: "12px 0",
    fontFamily: "var(--font-mono)",
  },
  ".cm-line": {
    padding: "0 14px",
  },
  ".cm-cursor, .cm-dropCursor": {
    borderLeftColor: "var(--editor-cursor)",
  },
  "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection":
    {
      backgroundColor: "var(--editor-selection)",
    },
  ".cm-activeLine": {
    backgroundColor: "var(--editor-active-line)",
  },
  ".cm-gutters": {
    backgroundColor: "var(--editor-gutter-bg)",
    color: "var(--editor-gutter-fg)",
    borderRight: "1px solid var(--border-hairline)",
    fontFamily: "var(--font-mono)",
    fontSize: "12px",
  },
  ".cm-activeLineGutter": {
    backgroundColor: "var(--editor-active-line)",
    color: "var(--editor-gutter-active-fg)",
  },
  ".cm-foldPlaceholder": {
    backgroundColor: "var(--surface-elevated)",
    border: "1px solid var(--border-strong)",
    color: "var(--fg-secondary)",
  },
  ".cm-tooltip": {
    backgroundColor: "var(--surface-elevated)",
    border: "1px solid var(--border-strong)",
    color: "var(--fg-primary)",
    borderRadius: "var(--radius-md)",
    boxShadow: "var(--shadow-popover)",
  },
  ".cm-tooltip-autocomplete > ul": {
    fontFamily: "var(--font-mono)",
    fontSize: "12.5px",
  },
  ".cm-tooltip-autocomplete ul li[aria-selected]": {
    backgroundColor: "var(--accent-subtle)",
    color: "var(--fg-primary)",
  },
  ".cm-completionDetail": {
    color: "var(--fg-muted)",
    fontStyle: "normal",
    marginLeft: "10px",
  },
  // The selected row is tinted, and muted text does not stand out on it.
  ".cm-tooltip-autocomplete ul li[aria-selected] .cm-completionDetail": {
    color: "var(--fg-secondary)",
  },
  ".cm-panels": {
    backgroundColor: "var(--surface-panel)",
    color: "var(--fg-primary)",
  },
  ".cm-searchMatch": {
    backgroundColor: "var(--status-warn-subtle)",
    outline: "1px solid var(--syntax-resource-border)",
  },
};
