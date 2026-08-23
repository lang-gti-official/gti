import type { Metadata } from "next";
import { notFound } from "next/navigation";
import diagnostics from "@/generated/diagnostics.json";
import reproducers from "@/generated/diagnostic-examples.json";
import { diagnosticProse } from "@/content/diagnostics";
import Code from "@/components/Code";

// One page per diagnostic code — every code the compiler can publish gets a
// stable URL from day one, which is the contract that lets the compiler emit
// `see: <url>` in its output. Documented codes carry authored prose plus a
// reproducer verified at build time to fail with exactly this code; the rest
// are explicit stubs, never guesses.

export function generateStaticParams() {
  return diagnostics.diagnostics.map((d) => ({ code: d.code }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ code: string }>;
}): Promise<Metadata> {
  const { code } = await params;
  const prose = diagnosticProse[code];
  return { title: prose ? `${code}: ${prose.summary}` : code };
}

export default async function DiagnosticPage({
  params,
}: {
  params: Promise<{ code: string }>;
}) {
  const { code } = await params;
  const diagnostic = diagnostics.diagnostics.find((d) => d.code === code);
  if (!diagnostic) notFound();

  const prose = diagnosticProse[code];
  const reproducer = (
    reproducers.reproducers as Record<
      string,
      { source: string; firstLine: string }
    >
  )[code];

  return (
    <article>
      <h1>
        <code>{diagnostic.code}</code>
        {prose ? <> — {prose.summary}</> : null}
      </h1>
      <p className="muted" style={{ fontSize: "0.8125rem" }}>
        Family: {diagnostic.family} · published from{" "}
        {diagnostic.files.map((file, index) => (
          <span key={file}>
            {index > 0 ? ", " : ""}
            <code>{file}</code>
          </span>
        ))}
      </p>

      {prose ? (
        <>
          <h2 id="rule">Rule</h2>
          <p>{prose.rule}</p>
          <h2 id="why">Why the rule exists</h2>
          <p>{prose.why}</p>
          <h2 id="fix">How to fix it</h2>
          <p>{prose.fix}</p>
        </>
      ) : (
        <div className="stub-notice">
          This code is published by the compiler but its reference page has
          not been written yet. The census above is generated, so the code
          itself is current; only the explanation is missing.
        </div>
      )}

      {reproducer ? (
        <>
          <h2 id="example">Example</h2>
          <p className="muted" style={{ fontSize: "0.8125rem" }}>
            Verified at build time to fail with exactly this code (
            {reproducers.verifiedWith}).
          </p>
          <Code code={reproducer.source} lang="gti" />
          <pre>
            <code>{reproducer.firstLine}</code>
          </pre>
        </>
      ) : null}
    </article>
  );
}
