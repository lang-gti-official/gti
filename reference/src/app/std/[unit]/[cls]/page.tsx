import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import classes from "@/generated/classes.json";
import examples from "@/generated/examples.json";
import { classDocs } from "@/content/std-classes";
import Code from "@/components/Code";

// One page per public standard-library class, in the cppreference shape:
// header line, description, then member tables grouped by category. Member
// signatures are quoted from the shipped stdlib source at build time.
//
// Staleness gate: a member documented in std-classes.ts that no longer
// exists in the extraction THROWS here, failing the static build. Members
// that exist but carry no prose are listed as undocumented, never hidden.

export function generateStaticParams() {
  return classes.classes.map((entry) => ({
    unit: entry.unit,
    cls: entry.name,
  }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ unit: string; cls: string }>;
}): Promise<Metadata> {
  const { unit, cls } = await params;
  const entry = classes.classes.find(
    (candidate) => candidate.unit === unit && candidate.name === cls,
  );
  return { title: entry ? entry.qualified : cls };
}

export default async function ClassPage({
  params,
}: {
  params: Promise<{ unit: string; cls: string }>;
}) {
  const { unit, cls } = await params;
  const entry = classes.classes.find(
    (candidate) => candidate.unit === unit && candidate.name === cls,
  );
  if (!entry) notFound();

  const doc = classDocs[`${unit}/${cls}`];
  const documented = new Set<string>();
  if (doc) {
    for (const category of doc.categories) {
      for (const member of category.members) {
        const exists = entry.members.some((m) => m.name === member.name);
        if (!exists) {
          throw new Error(
            `std-classes.ts documents ${entry.qualified}::${member.name}, ` +
              `but no such member exists in stdlib/std/${unit}.gti — ` +
              `the reference refuses to publish a stale member table.`,
          );
        }
        documented.add(member.name);
      }
    }
  }

  const undocumented = entry.members.filter(
    (member) => !documented.has(member.name),
  );
  const related = examples.examples.filter((e) => e.unit === unit);

  return (
    <article>
      <p className="muted" style={{ fontSize: "0.8125rem", margin: 0 }}>
        <Link href={`/std/${unit}/`}>
          <code>&lt;std/{unit}&gt;</code>
        </Link>{" "}
        · class
      </p>
      <h1>
        <code>{entry.qualified}</code>
      </h1>
      <p className="muted" style={{ fontSize: "0.875rem" }}>
        Defined in header <code>&lt;std/{unit}&gt;</code>
      </p>

      {doc ? (
        <>
          <p className="lede">{doc.brief}</p>
          {(doc.description ?? []).map((paragraph) => (
            <p key={paragraph.slice(0, 24)}>{paragraph}</p>
          ))}
        </>
      ) : (
        <div className="stub-notice">
          This class has no authored description yet. The member signatures
          below are quoted from the shipped source and are current.
        </div>
      )}

      {(doc?.categories ?? []).map((category) => (
        <section key={category.title}>
          <h2 id={category.title.toLowerCase().replace(/[^a-z]+/g, "-")}>
            {category.title}
          </h2>
          <table className="reference-table member-table">
            <tbody>
              {category.members.map((member) => {
                const signatures = entry.members.filter(
                  (m) => m.name === member.name,
                );
                return (
                  <tr key={member.name} id={member.name.replace(/[^a-zA-Z0-9_~]+/g, "-")}>
                    <td style={{ width: "34%" }}>
                      {signatures.map((signature) => (
                        <div key={signature.signature}>
                          <code>{signature.signature}</code>
                        </div>
                      ))}
                    </td>
                    <td>{member.brief}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </section>
      ))}

      {undocumented.length > 0 ? (
        <section>
          <h2 id="undocumented">
            {doc ? "Not yet documented" : "Members"}
          </h2>
          <table className="reference-table member-table">
            <tbody>
              {undocumented.map((member) => (
                <tr key={member.signature}>
                  <td>
                    <code>{member.signature}</code>
                  </td>
                </tr>
              ))}
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
