import type { Metadata } from "next";
import diagnostics from "@/generated/diagnostics.json";
import { diagnosticProse } from "@/content/diagnostics";
import { Page, Descriptions } from "@/components/Cpp";

export const metadata: Metadata = { title: "Diagnostics" };

// cppreference.com/w/cpp/error shape: description tables grouped by family.
// All codes the compiler can publish are listed — extracted from src/ at
// build time — so an undocumented code is visible rather than invisible.
export default function DiagnosticsIndex() {
  const families = new Map<string, typeof diagnostics.diagnostics>();
  for (const diagnostic of diagnostics.diagnostics) {
    const list = families.get(diagnostic.family) ?? [];
    list.push(diagnostic);
    families.set(diagnostic.family, list);
  }

  return (
    <Page title="Diagnostics">
      <p>
        GTI diagnostic codes are stable identifiers of the form{" "}
        <code>GTI-XNNNN</code>, where the letter names the compiler phase. All{" "}
        {diagnostics.count} codes the current compiler can publish are listed;
        each has a page, which is the target the compiler can cite in its
        output.
      </p>
      {[...families.entries()].map(([family, codes]) => (
        <div key={family}>
          <h3 id={family.toLowerCase().replace(/[^a-z]+/g, "-")}>{family}</h3>
          <Descriptions
            groups={[
              {
                rows: codes.map((diagnostic) => ({
                  name: diagnostic.code,
                  href: `/diagnostics/${diagnostic.code}/`,
                  description:
                    diagnosticProse[diagnostic.code]?.summary ??
                    "not yet described",
                  kind: diagnosticProse[diagnostic.code]
                    ? "diagnostic"
                    : "diagnostic, undocumented",
                })),
              },
            ]}
          />
        </div>
      ))}
    </Page>
  );
}
