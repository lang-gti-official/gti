import { highlight } from "@/lib/highlight";

// Server component: highlights at build time, ships plain HTML. Blocks with
// no recognized language (program output, compiler diagnostics) render as
// plain <pre> rather than being dressed up as code.
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
      <pre>
        <code>{code}</code>
      </pre>
    );
  }
  return <div dangerouslySetInnerHTML={{ __html: html }} />;
}
