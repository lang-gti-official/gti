import type { Metadata } from "next";
import Link from "next/link";
import diagnostics from "@/generated/diagnostics.json";
import { diagnosticProse } from "@/content/diagnostics";

export const metadata: Metadata = {
  title: "Diagnostics",
  description: "Every stable GTI diagnostic code.",
};

export default function DiagnosticsIndex() {
  const families = new Map<string, typeof diagnostics.diagnostics>();
  for (const diagnostic of diagnostics.diagnostics) {
    const list = families.get(diagnostic.family) ?? [];
    list.push(diagnostic);
    families.set(diagnostic.family, list);
  }

  return (
    <>
      <h1>Diagnostics</h1>
      <p className="lede">
        GTI diagnostic codes are stable identifiers. This census is extracted
        from the compiler source at build time — all{" "}
        <strong>{diagnostics.count}</strong> codes the implementation can
        publish are listed, so an undocumented diagnostic is visible here
        rather than invisible.
      </p>

      {[...families.entries()].map(([family, codes]) => (
        <section key={family}>
          <h2 id={family.toLowerCase().replace(/[^a-z]+/g, "-")}>
            {family}{" "}
            <span className="muted" style={{ fontWeight: 400 }}>
              ({codes.length})
            </span>
          </h2>
          <table className="reference-table">
            <tbody>
              {codes.map((diagnostic) => (
                <tr key={diagnostic.code} id={diagnostic.code}>
                  <td style={{ width: "9rem" }}>
                    <Link href={`/diagnostics/${diagnostic.code}/`}>
                      <code>{diagnostic.code}</code>
                    </Link>
                  </td>
                  <td className="muted">
                    {diagnosticProse[diagnostic.code] ? (
                      diagnosticProse[diagnostic.code].summary
                    ) : (
                      <span className="badge">to be documented</span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      ))}

      <div className="stub-notice">
        Every code has a stable page — the target of the{" "}
        <code>error[GTI-…]: see &lt;url&gt;</code> link the compiler will
        emit. Documented pages carry the rule, its rationale, and a
        reproducer verified at build time to fail with exactly that code.
      </div>
    </>
  );
}
