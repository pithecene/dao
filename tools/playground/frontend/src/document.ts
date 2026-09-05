// Identity of the document in the editor as the service reports it.
// Replies carry a `file` per position; positions whose file is not this
// one lie elsewhere in the program (the prelude today), which the
// single-document UI can name but not open.

let file = "";
let module = "";

export function setDocument(nextFile: string, nextModule: string): void {
  file = nextFile;
  module = nextModule;
  const label = document.getElementById("module-label");
  if (label) label.textContent = module ? `module ${module}` : "";
}

/** The service's display path for the editor buffer. */
export function documentFile(): string {
  return file;
}

/** True if a position with this `file` lies in the editor buffer. */
export function inDocument(positionFile: string): boolean {
  return positionFile === file;
}
