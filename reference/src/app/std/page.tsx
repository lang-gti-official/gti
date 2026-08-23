import type { Metadata } from "next";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";
import { Page, Descriptions } from "@/components/Cpp";

export const metadata: Metadata = { title: "Standard library headers" };

// cppreference.com/w/cpp/header: one description table of every header.
export default function HeadersPage() {
  return (
    <Page title="Standard library headers">
      <p>
        The GTI standard library is organized into units included as{" "}
        <code>#include &lt;std/name&gt;</code>. Each unit is independently
        parsed GTI source and does not re-export its own dependencies. This
        index is generated from <code>stdlib/std/</code> at build time.
      </p>
      <h3>Headers</h3>
      <Descriptions
        groups={[
          {
            rows: units.units.map((unit) => {
              const unitClasses = classes.classes.filter(
                (entry) => entry.unit === unit.name,
              );
              return {
                name: `<std/${unit.name}>`,
                href: `/std/${unit.name}/`,
                description:
                  unitClasses.length > 0
                    ? unitClasses.map((c) => c.qualified).join(", ")
                    : unit.summary.slice(0, 120) || "functions",
                kind: "header",
              };
            }),
          },
        ]}
      />
    </Page>
  );
}
