import { isActivePrelude, programRequest } from "./document";
import {
  autocompletion,
  type Completion,
  type CompletionContext,
  type CompletionResult,
} from "@codemirror/autocomplete";
import { api } from "./api";
import { getBufferGeneration } from "./editor";
import type { Completion as ServiceCompletion } from "./generated/tooling_surface";

/** CodeMirror completion icon for a service symbol kind. */
const ICON_BY_KIND: Record<string, string> = {
  function: "function",
  method: "method",
  field: "property",
  local: "variable",
  param: "variable",
  parameter: "variable",
  class: "class",
  alias: "class",
  type: "class",
  enum: "enum",
  variant: "enum",
  concept: "interface",
  module: "namespace",
};

function toOption(item: ServiceCompletion): Completion {
  return {
    label: item.label,
    type: ICON_BY_KIND[item.kind] ?? "variable",
    detail: item.type || undefined,
  };
}

async function daoCompletionSource(
  ctx: CompletionContext,
): Promise<CompletionResult | null> {
  if (isActivePrelude()) return null;

  const word = ctx.matchBefore(/[A-Za-z_][A-Za-z0-9_]*/);
  const from = word ? word.from : ctx.pos;
  const afterDot = from > 0 && ctx.state.sliceDoc(from - 1, from) === ".";
  if (!ctx.explicit && !word && !afterDot) return null;

  const generation = getBufferGeneration();
  const items = await api("completions", {
    ...programRequest(ctx.state.doc.toString()),
    offset: from,
  });
  // A swap to another file or example does not abort the context, and its
  // offsets would land in the buffer that replaced this one.
  if (generation !== getBufferGeneration()) return null;
  if (ctx.aborted || items.length === 0) return null;

  return {
    from,
    options: items.map(toOption),
    validFor: /^[A-Za-z0-9_]*$/,
  };
}

export const daoCompletions = autocompletion({
  override: [daoCompletionSource],
  activateOnTyping: true,
});
