import { editorThemeRules } from "./editor_theme";
import tokensSource from "./tokens.css?raw";

// Text must stand 4.5 to 1 against the surface behind it, in both
// themes.  The pairs below are the ones the stylesheet and the editor
// theme draw; each ratio is computed from the tokens, with a tint
// composited onto the surface under it, so a palette may change freely
// as long as the text stays legible.
//
// Not covered: text inside a selection, a reference highlight, or a
// search match.  Punctuation measures between 2.8 and 4.2 to 1 on those
// tints, and holding it there asks for a palette decision: punctuation
// as bright as keywords, or lighter tints.

const MINIMUM_RATIO = 4.5;

interface Color {
  r: number;
  g: number;
  b: number;
  alpha: number;
}

type Tokens = Map<string, string>;

/** The custom properties declared for one theme. */
function themeTokens(theme: string): Tokens {
  const tokens: Tokens = new Map();
  // A declaration inside a comment is not one the browser applies.
  const applied = tokensSource.replace(/\/\*[\s\S]*?\*\//g, "");
  for (const rule of applied.matchAll(/([^{}]+)\{([^}]*)\}/g)) {
    if (!rule[1].includes(`[data-theme="${theme}"]`)) continue;
    for (const declaration of rule[2].matchAll(/(--[\w-]+):\s*([^;]+);/g)) {
      tokens.set(declaration[1], declaration[2].trim());
    }
  }
  if (tokens.size === 0) throw new Error(`tokens.css declares no ${theme} theme`);
  return tokens;
}

function colorOf(tokens: Tokens, name: string): Color {
  const value = tokens.get(name);
  if (value === undefined) throw new Error(`tokens.css does not declare ${name}`);
  const reference = /^var\((--[\w-]+)\)$/.exec(value);
  if (reference) return colorOf(tokens, reference[1]);
  const hex = /^#([0-9a-f]{6})$/i.exec(value);
  if (hex) {
    const bits = parseInt(hex[1], 16);
    return { r: bits >> 16, g: (bits >> 8) & 255, b: bits & 255, alpha: 1 };
  }
  const rgba = /^rgba\(\s*([\d.]+),\s*([\d.]+),\s*([\d.]+),\s*([\d.]+)\s*\)$/.exec(value);
  if (rgba) {
    return { r: Number(rgba[1]), g: Number(rgba[2]), b: Number(rgba[3]), alpha: Number(rgba[4]) };
  }
  throw new Error(`${name} is not a colour this test reads: ${value}`);
}

function composite(top: Color, under: Color): Color {
  const mix = (above: number, below: number): number =>
    above * top.alpha + below * (1 - top.alpha);
  return { r: mix(top.r, under.r), g: mix(top.g, under.g), b: mix(top.b, under.b), alpha: 1 };
}

/** A text colour, which must be opaque: the ratio is measured on the token itself. */
function textColorOf(tokens: Tokens, name: string): Color {
  const color = colorOf(tokens, name);
  if (color.alpha !== 1) throw new Error(`${name} is a text colour and must be opaque`);
  return color;
}

/**
 * The token that colours the detail text of the selected completion row:
 * the theme's rule for that row, or without one the rule for every row,
 * as the cascade would choose.
 */
function selectedCompletionDetailToken(): string {
  const color =
    editorThemeRules[".cm-tooltip-autocomplete ul li[aria-selected] .cm-completionDetail"]
      ?.color ?? editorThemeRules[".cm-completionDetail"]?.color;
  const token = /^var\((--[\w-]+)\)$/.exec(color ?? "");
  if (!token) throw new Error(`the completion detail's colour is not a token: ${color}`);
  return token[1];
}

/** Relative luminance, as the accessibility guidelines define it. */
function luminance(color: Color): number {
  const linear = (channel: number): number => {
    const unit = channel / 255;
    return unit <= 0.03928 ? unit / 12.92 : ((unit + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

/** The surfaces text sits on: an opaque token, then the tints drawn over it. */
const PLAIN_SURFACES = [
  ["--surface-canvas"],
  ["--surface-base"],
  ["--surface-panel"],
  ["--surface-elevated"],
  ["--surface-subtle"],
  ["--surface-code"],
];
const SELECTED_COMPLETION = [["--surface-elevated", "--accent-subtle"]];
const EDITOR_LINES = [["--editor-bg"], ["--editor-bg", "--editor-active-line"]];
/** The editor's lines under a token's own background, which mode and resource names carry. */
const tintedLines = (tint: string): string[][] => EDITOR_LINES.map((line) => [...line, tint]);
const WARNING_BADGES = [
  ["--surface-base", "--status-warn-subtle"],
  ["--surface-panel", "--status-warn-subtle"],
  ["--surface-elevated", "--status-warn-subtle"],
];

const SYNTAX_COLORS = [
  "--syntax-keyword",
  "--syntax-type",
  "--syntax-decl",
  "--syntax-variable",
  "--syntax-field",
  "--syntax-module",
  "--syntax-lambda-param",
  "--syntax-number",
  "--syntax-string",
  "--syntax-operator",
  "--syntax-punctuation",
];

const PAIRINGS: { text: string[]; surfaces: string[][] }[] = [
  { text: ["--fg-primary", "--fg-secondary", "--fg-muted"], surfaces: PLAIN_SURFACES },
  { text: ["--fg-primary", selectedCompletionDetailToken()], surfaces: SELECTED_COMPLETION },
  { text: SYNTAX_COLORS, surfaces: EDITOR_LINES },
  { text: ["--syntax-mode"], surfaces: tintedLines("--syntax-mode-bg") },
  { text: ["--syntax-resource"], surfaces: tintedLines("--syntax-resource-bg") },
  { text: ["--editor-gutter-fg"], surfaces: [["--editor-gutter-bg"]] },
  {
    text: ["--editor-gutter-active-fg"],
    surfaces: [["--editor-gutter-bg", "--editor-active-line"]],
  },
  { text: ["--status-warn"], surfaces: WARNING_BADGES },
];

function run(): void {
  const failures: string[] = [];
  for (const theme of ["dark", "light"]) {
    const tokens = themeTokens(theme);
    for (const { text: names, surfaces } of PAIRINGS) {
      for (const layers of surfaces) {
        const surface = layers
          .map((layer) => colorOf(tokens, layer))
          .reduce((under, top) => composite(top, under));
        for (const name of names) {
          const text = luminance(textColorOf(tokens, name));
          const lighter = Math.max(text, luminance(surface));
          const darker = Math.min(text, luminance(surface));
          const ratio = (lighter + 0.05) / (darker + 0.05);
          if (ratio < MINIMUM_RATIO) {
            failures.push(`${theme}: ${name} on ${layers.join(" + ")} is ${ratio.toFixed(2)} to 1`);
          }
        }
      }
    }
  }
  if (failures.length > 0) {
    throw new Error(`text under ${MINIMUM_RATIO} to 1:\n  ${failures.join("\n  ")}`);
  }
  console.log("contrast.test: all assertions passed");
}

run();
