// Generated from compiler/analysis/tooling_surface.h by tooling_surface_dump.
// Do not edit by hand: change the table and run `task gen-tooling-surface`.
// tooling_surface_test fails while this file and the table disagree.

export const TOKEN_KINDS = [
  "keyword.module",
  "keyword.import",
  "keyword.type",
  "keyword.extern",
  "keyword.fn",
  "keyword.let",
  "keyword.return",
  "keyword.if",
  "keyword.else",
  "keyword.while",
  "keyword.for",
  "keyword.in",
  "keyword.mode",
  "keyword.resource",
  "mode.unsafe",
  "mode.gpu",
  "mode.parallel",
  "resource.kind.memory",
  "resource.binding",
  "type.builtin",
  "type.nominal",
  "decl.function",
  "decl.type",
  "decl.field",
  "decl.module",
  "use.function",
  "use.variable.local",
  "use.variable.param",
  "use.field",
  "use.module",
  "literal.string",
  "literal.number",
  "operator.pipe",
  "operator.arrow",
  "operator.context",
  "operator.assignment",
  "operator.namespace",
  "punctuation",
  "lambda.param",
  "decl.variable.local",
  "decl.variable.param",
  "keyword.concept",
  "keyword.derived",
  "keyword.extend",
  "keyword.deny",
  "keyword.as",
  "keyword.self",
  "keyword.where",
  "keyword.match",
  "keyword.break",
  "keyword.yield",
  "operator.arithmetic",
  "operator.comparison",
  "operator.logical",
  "operator.member",
  "operator.range",
  "operator.address",
  "operator.try",
  "literal.bool",
  "use.type",
  "use.variant",
] as const;

export type TokenKind = (typeof TOKEN_KINDS)[number];

export type TokenGroup = "keyword" | "mode" | "resource" | "type" | "decl" | "variable" | "field" | "module" | "lambda-param" | "literal-number" | "literal-string" | "operator" | "punctuation";

/** Visual group of every token kind; the CSS class is `dao-<group>`. */
export const TOKEN_GROUP: Record<TokenKind, TokenGroup> = {
  "keyword.module": "keyword",
  "keyword.import": "keyword",
  "keyword.type": "keyword",
  "keyword.extern": "keyword",
  "keyword.fn": "keyword",
  "keyword.let": "keyword",
  "keyword.return": "keyword",
  "keyword.if": "keyword",
  "keyword.else": "keyword",
  "keyword.while": "keyword",
  "keyword.for": "keyword",
  "keyword.in": "keyword",
  "keyword.mode": "keyword",
  "keyword.resource": "keyword",
  "mode.unsafe": "mode",
  "mode.gpu": "mode",
  "mode.parallel": "mode",
  "resource.kind.memory": "resource",
  "resource.binding": "resource",
  "type.builtin": "type",
  "type.nominal": "type",
  "decl.function": "decl",
  "decl.type": "decl",
  "decl.field": "field",
  "decl.module": "module",
  "use.function": "decl",
  "use.variable.local": "variable",
  "use.variable.param": "variable",
  "use.field": "field",
  "use.module": "module",
  "literal.string": "literal-string",
  "literal.number": "literal-number",
  "operator.pipe": "operator",
  "operator.arrow": "operator",
  "operator.context": "operator",
  "operator.assignment": "operator",
  "operator.namespace": "operator",
  "punctuation": "punctuation",
  "lambda.param": "lambda-param",
  "decl.variable.local": "variable",
  "decl.variable.param": "variable",
  "keyword.concept": "keyword",
  "keyword.derived": "keyword",
  "keyword.extend": "keyword",
  "keyword.deny": "keyword",
  "keyword.as": "keyword",
  "keyword.self": "keyword",
  "keyword.where": "keyword",
  "keyword.match": "keyword",
  "keyword.break": "keyword",
  "keyword.yield": "keyword",
  "operator.arithmetic": "operator",
  "operator.comparison": "operator",
  "operator.logical": "operator",
  "operator.member": "operator",
  "operator.range": "operator",
  "operator.address": "operator",
  "operator.try": "operator",
  "literal.bool": "literal-number",
  "use.type": "type",
  "use.variant": "field",
};

export type LexicalCategory = "keyword" | "identifier" | "literal.number" | "literal.string" | "literal.bool" | "operator" | "punctuation" | "synthetic" | "error" | "unknown";

export type DiagnosticSeverity = "error" | "warning";

export interface LexToken {
  kind: string;
  category: LexicalCategory;
  offset: number;
  length: number;
  line: number;
  col: number;
  text: string;
}

export interface SemanticToken {
  kind: TokenKind;
  offset: number;
  length: number;
  line: number;
  col: number;
}

export interface Diagnostic {
  severity: DiagnosticSeverity;
  offset: number;
  length: number;
  line: number;
  col: number;
  message: string;
}

export interface SourceRequest {
  source: string;
}

export interface AnalyzeRequest {
  source: string;
  includePrelude?: boolean;
}

export interface PositionRequest {
  source: string;
  offset: number;
}

export interface ExampleName {
  name: string;
}

export interface AnalyzeResponse {
  tokens: LexToken[];
  semanticTokens: SemanticToken[];
  ast: string;
  hir: string;
  mir: string;
  llvm_ir: string;
  diagnostics: Diagnostic[];
}

export interface RunResponse {
  stdout: string;
  stderr: string;
  exit_code: number;
  diagnostics: Diagnostic[];
}

export interface Hover {
  name: string;
  kind: string;
  type: string;
}

export interface Definition {
  offset: number;
  length: number;
  line: number;
  col: number;
}

export interface DocumentSymbol {
  name: string;
  kind: string;
  offset: number;
  length: number;
  children: DocumentSymbol[];
}

export interface Reference {
  offset: number;
  length: number;
  isDefinition: boolean;
}

export interface Completion {
  label: string;
  kind: string;
  type: string;
}

export interface ExampleEntry {
  name: string;
}

export interface ExamplesList {
  examples: ExampleEntry[];
}

export interface ExampleSource {
  name: string;
  source: string;
}

export interface ErrorReply {
  error: string;
}

export const ROUTES = {
  analyze: { method: "POST", path: "/api/analyze" },
  run: { method: "POST", path: "/api/run" },
  hover: { method: "POST", path: "/api/hover" },
  gotoDef: { method: "POST", path: "/api/goto-def" },
  documentSymbols: { method: "POST", path: "/api/document-symbols" },
  references: { method: "POST", path: "/api/references" },
  completions: { method: "POST", path: "/api/completions" },
  examples: { method: "GET", path: "/api/examples" },
  example: { method: "GET", path: "/api/examples/:name" },
} as const;

export type RouteName = keyof typeof ROUTES;

/** Request body and response type of every route. */
export interface RouteTypes {
  analyze: { request: AnalyzeRequest; response: AnalyzeResponse };
  run: { request: SourceRequest; response: RunResponse };
  hover: { request: PositionRequest; response: Hover | null };
  gotoDef: { request: PositionRequest; response: Definition | null };
  documentSymbols: { request: SourceRequest; response: DocumentSymbol[] };
  references: { request: PositionRequest; response: Reference[] };
  completions: { request: PositionRequest; response: Completion[] };
  examples: { request: void; response: ExamplesList };
  example: { request: ExampleName; response: ExampleSource };
}
