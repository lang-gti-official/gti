import type { Metadata } from "next";
import { notFound } from "next/navigation";
import units from "@/generated/units.json";
import examples from "@/generated/examples.json";

// One page per standard-library unit. The synopsis is quoted verbatim from
// the shipped stdlib source at build time, and every example shown was
// compiled and executed by the same compiler that owns that source — the
// output block is the real program's real output.

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

  const related = examples.examples.filter((e) => e.unit === unit.name);

  return (
    <article>
      <h1>
        <code>{unit.include}</code>
      </h1>
      {unit.summary ? <p className="lede">{unit.summary}</p> : null}

      {unit.types.length > 0 ? (
        <p>
          Public types:{" "}
          {unit.types.map((typeName, index) => (
            <span key={typeName}>
              {index > 0 ? ", " : ""}
              <code>{typeName}</code>
            </span>
          ))}
        </p>
      ) : null}

      {unit.synopsis ? (
        <>
          <h2 id="synopsis">Synopsis</h2>
          <p className="muted" style={{ fontSize: "0.8125rem" }}>
            Public declarations quoted from{" "}
            <code>stdlib/std/{unit.name}.gti</code> at build time.
          </p>
          <pre>
            <code>{unit.synopsis}</code>
          </pre>
        </>
      ) : null}

      {related.map((example) => (
        <section key={example.name}>
          <h2 id={example.name}>Example: {example.title}</h2>
          <pre>
            <code>{example.source}</code>
          </pre>
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
