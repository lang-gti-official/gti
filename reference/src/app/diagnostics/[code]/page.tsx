import type { Metadata } from "next";
import { notFound } from "next/navigation";
import diagnostics from "@/generated/diagnostics.json";
import reproducers from "@/generated/diagnostic-examples.json";
import { diagnosticProse } from "@/content/diagnostics";
import { Page, Descriptions } from "@/components/Cpp";
import Code from "@/components/Code";

// One page per diagnostic code — the stable URL the compiler can cite.
// Documented codes carry authored rule/rationale/fix prose and a reproducer
// verified at build time to fail with exactly this code.

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
    reproducers.reproducers as Record<string, { source: string; firstLine: string }>
  )[code];

  return (
    <Page title={prose ? `${code}: ${prose.summary}` : code}>
      <table className="t-dcl-begin">
        <tbody>
          <tr className="t-dsc-header">
            <td>
              <div>
                Published by the {diagnostic.family.toLowerCase()} phase from{" "}
                {diagnostic.files.map((file, index) => (
                  <span key={file}>
                    {index > 0 ? ", " : ""}
                    <code>{file}</code>
                  </span>
                ))}
              </div>
            </td>
            <td></td>
            <td></td>
          </tr>
        </tbody>
      </table>

      {prose ? (
        <>
          <h3>Rule</h3>
          <p>{prose.rule}</p>
          <h3>Rationale</h3>
          <p>{prose.why}</p>
          <h3>How to fix it</h3>
          <p>{prose.fix}</p>
        </>
      ) : (
        <p>
          This code is published by the compiler but its reference page has
          not been written yet. The census above is generated, so the code
          itself is current; only the explanation is missing.
        </p>
      )}

      {reproducer ? (
        <>
          <h3>Example</h3>
          <div className="t-example">
            <div className="t-example-live-link">
              <span className="coliru-btn">
                Verified to fail with {code} ({reproducers.verifiedWith})
              </span>
            </div>
            <Code code={reproducer.source} lang="gti" />
            <p>Output:</p>
            <div className="mw-highlight">
              <pre>{reproducer.firstLine}</pre>
            </div>
          </div>
        </>
      ) : null}

      <h3>See also</h3>
      <Descriptions
        groups={[
          {
            rows: [
              {
                name: "Diagnostics",
                href: "/diagnostics/",
                description: `all ${diagnostics.count} stable codes by family`,
                kind: "index",
              },
            ],
          },
        ]}
      />
    </Page>
  );
}
