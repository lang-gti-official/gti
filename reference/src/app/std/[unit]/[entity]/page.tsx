import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import classes from "@/generated/classes.json";
import functions from "@/generated/functions.json";
import examples from "@/generated/examples.json";
import { classDocs } from "@/content/std-classes";
import { functionDocs } from "@/content/std-functions";
import { Page, Declarations, Descriptions, Example } from "@/components/Cpp";
import { memberSlug } from "@/lib/slug";

// One page per class or free function — cppreference.com/w/cpp/container/
// vector and cppreference.com/w/cpp/algorithm/min respectively. A class page
// is: declaration box, description, "Member functions" description tables
// grouped by category (each row linking to its member page), example. A
// function page is: numbered overload declarations, description.
//
// Staleness gate: authored docs naming a member that no longer exists in the
// extraction throw here and fail the static build.

export function generateStaticParams() {
  const params: { unit: string; entity: string }[] = [];
  for (const entry of classes.classes) {
    params.push({ unit: entry.unit, entity: entry.name });
  }
  for (const unit of functions.units) {
    for (const name of new Set(unit.functions.map((f) => f.name))) {
      if (!classes.classes.some((c) => c.unit === unit.unit && c.name === name)) {
        params.push({ unit: unit.unit, entity: name });
      }
    }
  }
  return params;
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ unit: string; entity: string }>;
}): Promise<Metadata> {
  const { unit, entity } = await params;
  const cls = classes.classes.find((c) => c.unit === unit && c.name === entity);
  return { title: cls ? cls.qualified : `std::${entity}` };
}

function memberKind(signature: string, className: string, name: string) {
  if (name === className) return "public member function";
  if (name.startsWith("~")) return "public member function";
  if (signature.startsWith("static constexpr")) return "public static member constant";
  if (signature.startsWith("static ")) return "public static member function";
  if (signature.startsWith("using ")) return "public member type";
  return "public member function";
}

export default async function EntityPage({
  params,
}: {
  params: Promise<{ unit: string; entity: string }>;
}) {
  const { unit, entity } = await params;
  const cls = classes.classes.find((c) => c.unit === unit && c.name === entity);
  const header = { label: `<std/${unit}>`, href: `/std/${unit}/` };

  if (cls) {
    const doc = classDocs[`${unit}/${entity}`];
    const documented = new Set<string>();
    for (const category of doc?.categories ?? []) {
      for (const member of category.members) {
        if (!cls.members.some((m) => m.name === member.name)) {
          throw new Error(
            `std-classes.ts documents ${cls.qualified}::${member.name}, but no ` +
              `such member exists in stdlib/std/${unit}.gti — the reference ` +
              `refuses to publish a stale member table.`,
          );
        }
        documented.add(member.name);
      }
    }
    const undocumentedNames = [
      ...new Set(cls.members.map((m) => m.name)),
    ].filter((name) => !documented.has(name));
    const related = examples.examples.filter((e) => e.unit === unit);

    const row = (name: string, brief: string) => {
      const first = cls.members.find((m) => m.name === name)!;
      return {
        name: name === cls.name ? `(constructor)` : name.startsWith("~") ? "(destructor)" : name,
        href: `/std/${unit}/${cls.name}/${memberSlug(name)}/`,
        anchor: memberSlug(name),
        description: brief,
        kind: memberKind(first.signature, cls.name, name),
      };
    };

    return (
      <Page title={cls.qualified}>
        <Declarations header={header} declarations={[{ code: cls.declaration }]} />
        {doc ? (
          <>
            <p>{doc.brief}</p>
            {(doc.description ?? []).map((paragraph) => (
              <p key={paragraph.slice(0, 24)}>{paragraph}</p>
            ))}
          </>
        ) : (
          <p>
            <code>{cls.qualified}</code> is declared in{" "}
            <code>&lt;std/{unit}&gt;</code>. Its description has not been
            written yet; the member list below is quoted from the shipped
            source and is current.
          </p>
        )}

        {doc?.cppDifferences ? (
          <>
            <h3>Differences from C++</h3>
            <ul>
              {doc.cppDifferences.map((difference) => (
                <li key={difference.slice(0, 24)}>{difference}</li>
              ))}
            </ul>
          </>
        ) : null}

        <h3>Member functions</h3>
        <Descriptions
          groups={[
            ...(doc?.categories ?? []).map((category) => ({
              heading: category.title,
              rows: category.members.map((m) => row(m.name, m.brief)),
            })),
            ...(undocumentedNames.length > 0
              ? [
                  {
                    heading: doc ? "Not yet described" : undefined,
                    rows: undocumentedNames.map((name) =>
                      row(name, "not yet described"),
                    ),
                  },
                ]
              : []),
          ]}
        />

        {related.map((example) => (
          <div key={example.name}>
            <h3>Example</h3>
            <Example
              source={example.source}
              output={example.output}
              verifiedWith={examples.verifiedWith}
            />
          </div>
        ))}

        <h3>See also</h3>
        <Descriptions
          groups={[
            {
              rows: [
                {
                  name: `<std/${unit}>`,
                  href: `/std/${unit}/`,
                  description: "the header this class is defined in",
                  kind: "header",
                },
              ],
            },
          ]}
        />
      </Page>
    );
  }

  const unitFunctions =
    functions.units.find((u) => u.unit === unit)?.functions ?? [];
  const overloads = unitFunctions.filter((f) => f.name === entity);
  if (overloads.length === 0) notFound();
  const brief = (functionDocs[unit] ?? []).find((d) => d.name === entity)?.brief;

  return (
    <Page title={`std::${entity}`}>
      <Declarations
        header={header}
        declarations={overloads.map((f) => ({ code: f.signature }))}
      />
      <p>
        {brief ??
          "This function's description has not been written yet; the declarations above are quoted from the shipped source and are current."}
      </p>
      <h3>See also</h3>
      <Descriptions
        groups={[
          {
            rows: [
              {
                name: `<std/${unit}>`,
                href: `/std/${unit}/`,
                description: "the header this function is defined in",
                kind: "header",
              },
            ],
          },
        ]}
      />
    </Page>
  );
}
