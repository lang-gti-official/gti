import { highlight } from "@/lib/highlight";

// Server component: highlights at build time, ships plain HTML inside the
// cppreference .mw-highlight box. Blocks with no recognized language
// (program output, compiler diagnostics) render as a plain box.
export default async function Code({
  code,
  lang,
}: {
  code: string;
  lang?: string;
}) {
  const html = await highlight(code, lang);
  if (!html) {
    return (
      <div className="mw-highlight">
        <pre>{code}</pre>
      </div>
    );
  }
  return (
    <div
      className="mw-highlight"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
}
