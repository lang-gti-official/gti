import type { Metadata } from "next";
import { notFound } from "next/navigation";
import classes from "@/generated/classes.json";
import { classDocs } from "@/content/std-classes";
import { Page, Declarations, Descriptions } from "@/components/Cpp";
import { memberSlug } from "@/lib/slug";

// One page per class member — cppreference.com/w/cpp/container/vector/
// push_back: the numbered overload declarations quoted from the shipped
// source, the authored description, and a See also back to the class.

export function generateStaticParams() {
  const params: { unit: string; entity: string; member: string }[] = [];
  for (const entry of classes.classes) {
    for (const name of new Set(entry.members.map((m) => m.name))) {
      params.push({ unit: entry.unit, entity: entry.name, member: memberSlug(name) });
    }
  }
  return params;
}

function resolve(unit: string, entity: string, member: string) {
  const cls = classes.classes.find((c) => c.unit === unit && c.name === entity);
  if (!cls) return null;
  const name = [...new Set(cls.members.map((m) => m.name))].find(
    (candidate) => memberSlug(candidate) === member,
  );
  if (!name) return null;
  return { cls, name, overloads: cls.members.filter((m) => m.name === name) };
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ unit: string; entity: string; member: string }>;
}): Promise<Metadata> {
  const { unit, entity, member } = await params;
  const found = resolve(unit, entity, member);
  return { title: found ? `${found.cls.qualified}::${found.name}` : member };
}

export default async function MemberPage({
  params,
}: {
  params: Promise<{ unit: string; entity: string; member: string }>;
}) {
  const { unit, entity, member } = await params;
  const found = resolve(unit, entity, member);
  if (!found) notFound();
  const { cls, name, overloads } = found;

  const doc = classDocs[`${unit}/${entity}`];
  const brief = doc?.categories
    .flatMap((category) => category.members)
    .find((m) => m.name === name)?.brief;

  const displayName =
    name === cls.name ? `${cls.qualified}::${cls.name}` :
    name.startsWith("~") ? `${cls.qualified}::~${cls.name}` :
    `${cls.qualified}::${name}`;

  return (
    <Page title={displayName}>
      <Declarations
        header={{ label: `<std/${unit}>`, href: `/std/${unit}/` }}
        declarations={overloads.map((m) => ({ code: m.signature }))}
      />
      <p>
        {brief ??
          "This member's description has not been written yet; the declarations above are quoted from the shipped source and are current."}
      </p>
      <h3>See also</h3>
      <Descriptions
        groups={[
          {
            rows: [
              {
                name: cls.qualified,
                href: `/std/${unit}/${cls.name}/`,
                description: doc?.brief ?? "the owning class",
                kind: "class",
              },
            ],
          },
        ]}
      />
    </Page>
  );
}
