import type { Metadata } from "next";
import diagnostics from "@/generated/diagnostics.json";

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
                    <code>{diagnostic.code}</code>
                  </td>
                  <td className="muted">
                    <span className="badge">
                      to be documented
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      ))}

      <div className="stub-notice">
        Each code becomes a page carrying the rule it enforces, why the rule
        exists, and a compiling example — the target of the{" "}
        <code>error[GTI-…]: see &lt;url&gt;</code> link the compiler will emit.
      </div>
    </>
  );
}
