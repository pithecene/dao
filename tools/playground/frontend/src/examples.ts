import { api } from "./api";
import { setSource } from "./editor";
import { doAnalyze } from "./analysis";

const FALLBACK_SOURCE = `fn add(a: i32, b: i32): i32 -> a + b

fn main(): i32
    let x: i32 = add(1, 2)
    return x
`;

/**
 * Load the example list, populate the dropdown, and auto-load hello.dao
 * (or the first available example) into the editor.
 */
export async function loadExamples(): Promise<void> {
  const select = document.getElementById(
    "example-select",
  ) as HTMLSelectElement;

  try {
    const { examples } = await api("examples", undefined);

    for (const example of examples) {
      const option = document.createElement("option");
      option.value = example.name;
      option.textContent = example.name;
      select.appendChild(option);
    }

    const defaultName =
      examples.find((e) => e.name === "hello.dao")?.name ?? examples[0]?.name;

    if (defaultName) {
      await loadExample(defaultName, select);
    } else {
      setSource(FALLBACK_SOURCE);
      doAnalyze();
    }

    select.addEventListener("change", async () => {
      const name = select.value;
      if (!name) return;
      await loadExample(name, select);
    });
  } catch (err) {
    console.error("failed to load example list:", err);
    setSource(FALLBACK_SOURCE);
    doAnalyze();
  }
}

async function loadExample(
  name: string,
  select: HTMLSelectElement,
): Promise<void> {
  try {
    const data = await api("example", { name });
    setSource(data.source);
    select.value = name;
    doAnalyze();
  } catch (err) {
    console.error("failed to load example:", err);
  }
}
