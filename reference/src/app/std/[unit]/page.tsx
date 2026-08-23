import type { Metadata } from "next";
import { notFound } from "next/navigation";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";
import functions from "@/generated/functions.json";
import examples from "@/generated/examples.json";
import { classDocs } from "@/content/std-classes";
import { functionDocs } from "@/content/std-functions";
import { Page, Descriptions, Example } from "@/components/Cpp";
import Code from "@/components/Code";

// cppreference.com/w/cpp/header/<name>: Classes and Functions description
// tables, examples, then the full Synopsis at the bottom. Signatures are
// quoted from the shipped source; a documented function that does not exist
// fails the static build.

export function generateStaticParams() {
  return units.units.map((unit) => ({ unit: unit.name }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ unit: string }>;
}): Promise<Metadata> {
  const { unit } = await params;
  return { title: `Standard library header <std/${unit}>` };
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
  const extracted = new Set(unitFunctions.map((f) => f.name));
  for (const doc of briefs) {
    if (!extracted.has(doc.name)) {
      throw new Error(
        `std-functions.ts documents std::${doc.name} in <std/${unit.name}>, ` +
          `but no such function exists in stdlib/std/${unit.name}.gti — ` +
          `the reference refuses to publish a stale function table.`,
      );
    }
  }
  const briefed = new Map(briefs.map((doc) => [doc.name, doc.brief]));
  const functionNames = [...new Set(unitFunctions.map((f) => f.name))].sort(
    (a, b) =>
      Number(!briefed.has(a)) - Number(!briefed.has(b)) || a.localeCompare(b),
  );
  const related = examples.examples.filter((e) => e.unit === unit.name);

  return (
    <Page
      title={
        <>
          Standard library header <code>&lt;std/{unit.name}&gt;</code>
        </>
      }
    >
      {unit.summary ? <p>{unit.summary}</p> : null}

      {unitClasses.length > 0 ? (
        <>
          <h3>Classes</h3>
          <Descriptions
            groups={[
              {
                rows: unitClasses.map((entry) => ({
                  name: entry.qualified,
                  href: `/std/${unit.name}/${entry.name}/`,
                  description:
                    classDocs[`${unit.name}/${entry.name}`]?.brief ??
                    "not yet described",
                  kind: "class",
                })),
              },
            ]}
          />
        </>
      ) : null}

      {functionNames.length > 0 ? (
        <>
          <h3>Functions</h3>
          <Descriptions
            groups={[
              {
                rows: functionNames.map((name) => ({
                  name,
                  href: `/std/${unit.name}/${name}/`,
                  description: briefed.get(name) ?? "not yet described",
                  kind: "function",
                })),
              },
            ]}
          />
        </>
      ) : null}

      {related.map((example) => (
        <div key={example.name}>
          <h3>Example: {example.title}</h3>
          <Example
            source={example.source}
            output={example.output}
            verifiedWith={examples.verifiedWith}
          />
        </div>
      ))}

      {unit.synopsis ? (
        <>
          <h3>Synopsis</h3>
          <Code code={unit.synopsis} lang="gti" />
        </>
      ) : null}
    </Page>
  );
}
