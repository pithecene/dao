// A `?raw` import is the file's text, inlined when the site is built.
declare module "*.dao?raw" {
  const source: string;
  export default source;
}
