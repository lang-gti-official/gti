import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";
import functions from "@/generated/functions.json";
import examples from "@/generated/examples.json";
import { classDocs } from "@/content/std-classes";
import { functionDocs } from "@/content/std-functions";
import Breadcrumbs from "@/components/Breadcrumbs";
import Code from "@/components/Code";

// One page per header, in the cppreference shape: classes table, functions
// table grouped with briefs, then verified examples. Signatures are quoted
// from the shipped source; authored briefs naming a function that does not
// exist fail the static build.

export function generateStaticParams() {
  return units.units.map((unit) => ({ unit: unit.name }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ unit: string }>;
}): Promise<Metadata> {
  const { unit } = await params;
  return { title: `<std/${unit}>` };
}

export default async function UnitPage({
  params,
}: {
  params: Promise<{ unit: string }>;
}) {
  const { unit: unitName } = await params;
  const unit = units.units.find((u) => u.name === unitName);
  if (!unit) notFound();

  const unitClasses = classes.classes.filter((c) => c.unit === unit.name);
  const unitFunctions =
    functions.units.find((u) => u.unit === unit.name)?.functions ?? [];
  const briefs = functionDocs[unit.name] ?? [];

  const extractedNames = new Set(unitFunctions.map((f) => f.name));
  for (const doc of briefs) {
    if (!extractedNames.has(doc.name)) {
      throw new Error(
        `std-functions.ts documents std::${doc.name} in <std/${unit.name}>, ` +
          `but no such function exists in stdlib/std/${unit.name}.gti — ` +
          `the reference refuses to publish a stale function table.`,
      );
    }
  }
  const briefed = new Map(briefs.map((doc) => [doc.name, doc.brief]));
  const functionNames = [...new Set(unitFunctions.map((f) => f.name))].sort(
    (a, b) => {
      const aDocumented = briefed.has(a) ? 0 : 1;
      const bDocumented = briefed.has(b) ? 0 : 1;
      return aDocumented - bDocumented || a.localeCompare(b);
    },
  );

  const related = examples.examples.filter((e) => e.unit === unit.name);

  return (
    <article>
      <Breadcrumbs
        trail={[{ label: "std", href: "/std/" }, { label: unit.name }]}
      />
      <h1>
        <code>{unit.include}</code>
      </h1>
      {unit.summary ? <p className="lede">{unit.summary}</p> : null}

      {unitClasses.length > 0 ? (
        <section>
          <h2 id="classes">Classes</h2>
          <table className="reference-table">
            <tbody>
              {unitClasses.map((entry) => (
                <tr key={entry.name}>
                  <td style={{ width: "16rem" }}>
                    <Link href={`/std/${unit.name}/${entry.name}/`}>
                      <code>{entry.qualified}</code>
                    </Link>
                  </td>
                  <td className="muted">
                    {classDocs[`${unit.name}/${entry.name}`]?.brief ?? (
                      <span className="badge">to be documented</span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      ) : null}

      {functionNames.length > 0 ? (
        <section>
          <h2 id="functions">Functions</h2>
          <table className="reference-table member-table">
            <tbody>
              {functionNames.map((name) => {
                const signatures = unitFunctions.filter(
                  (f) => f.name === name,
                );
                return (
                  <tr key={name} id={name}>
                    <td style={{ width: "44%" }}>
                      {signatures.map((f) => (
                        <div key={f.signature}>
                          <code>{f.signature}</code>
                        </div>
                      ))}
                    </td>
                    <td className="muted">
                      {briefed.get(name) ?? (
                        <span className="badge">to be documented</span>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </section>
      ) : null}

      {related.map((example) => (
        <section key={example.name}>
          <h2 id={example.name}>Example: {example.title}</h2>
          <Code code={example.source} lang="gti" />
          <p className="muted" style={{ fontSize: "0.8125rem", marginBottom: "0.25rem" }}>
            Output ({examples.verifiedWith}):
          </p>
          <pre>
            <code>{example.output}</code>
          </pre>
        </section>
      ))}
    </article>
  );
}
