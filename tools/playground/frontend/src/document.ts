// Identity of the document in the editor as the service reports it.
// Requests are program-shaped (files plus the document they are about);
// this UI sends one file.  Replies carry a `file` per position; positions
// whose file is not this one lie elsewhere in the program (the prelude
// today), which the single-document UI can name but not open.

/** The path this UI gives its one document inside the program it sends. */
export const DOCUMENT_PATH = "<playground>";

let file = "";
let module = "";

/** The program a request carries: this UI's single document, named. */
export function programRequest(source: string): {
  files: { path: string; source: string }[];
  document: string;
} {
  return { files: [{ path: DOCUMENT_PATH, source }], document: DOCUMENT_PATH };
}

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
