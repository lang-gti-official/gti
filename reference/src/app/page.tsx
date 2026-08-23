import Link from "next/link";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";
import diagnostics from "@/generated/diagnostics.json";
import language from "@/generated/language.json";
import version from "@/generated/version.json";

// The main page in the cppreference.com/w/cpp layout: a top row of release
// links, then a mainpagetable whose cells are bold area headings over dense
// runs of links separated by " − " and line breaks.

const UNIT_GROUPS: { title: string; units: string[] }[] = [
  { title: "Containers", units: ["array", "vector", "list", "forward_list", "span"] },
  { title: "Strings", units: ["string", "format"] },
  { title: "Numerics", units: ["numeric", "cmath", "random"] },
  { title: "Algorithms and iterators", units: ["algorithm", "iterator"] },
  { title: "Utilities", units: ["utility", "optional", "functional", "memory"] },
  { title: "Input/output and system", units: ["cstdio", "filesystem", "tcp"] },
];

function Run({ items }: { items: { label: React.ReactNode; href: string }[] }) {
  return (
    <>
      {items.map((item, index) => (
        <span key={item.href}>
          {index > 0 ? " − " : ""}
          <Link href={item.href}>{item.label}</Link>
        </span>
      ))}
    </>
  );
}

export default function Home() {
  const families = [...new Set(diagnostics.diagnostics.map((d) => d.family))];
  const sections = language.sections.map((section) => ({
    label: section.title.replace(/^\d+\.\s*/, ""),
    href: `/language/${section.slug}/`,
  }));

  return (
    <>
      <h1 id="firstHeading">GTI reference</h1>
      <div id="mw-content-text">
        <table className="mainpagetable">
          <tbody>
            <tr className="row rowtop">
              <td colSpan={3}>
                <p>
                  <span style={{ fontSize: "0.8em" }}>
                    {version.gtiVersion}
                    {"  │  "}
                    <a href="https://github.com/LukeHHA/gti/releases">Releases</a>
                    {"  │  "}
                    <Link href="/about/">About this reference</Link>
                  </span>
                </p>
              </td>
            </tr>
            <tr className="row">
              <td>
                <p>
                  <b>
                    <Link href="/language/">Language</Link>
                  </b>
                </p>
                <div className="mainpagediv">
                  {sections.map((section) => (
                    <p key={section.href} style={{ margin: "1px 0" }}>
                      <Link href={section.href}>{section.label}</Link>
                    </p>
                  ))}
                </div>
              </td>
              <td>
                <p>
                  <b>
                    <Link href="/std/">Standard library</Link>
                  </b>
                </p>
                <div className="mainpagediv">
                  {UNIT_GROUPS.map((group) => {
                    const present = group.units.filter((name) =>
                      units.units.some((u) => u.name === name),
                    );
                    if (present.length === 0) return null;
                    return (
                      <p key={group.title} style={{ margin: "3px 0" }}>
                        <b style={{ fontWeight: 400, color: "#555" }}>
                          {group.title}
                        </b>
                        <br />
                        <Run
                          items={present.map((name) => ({
                            label: <code>&lt;std/{name}&gt;</code>,
                            href: `/std/${name}/`,
                          }))}
                        />
                      </p>
                    );
                  })}
                </div>
              </td>
              <td>
                <p>
                  <b>Classes</b>
                </p>
                <div className="mainpagediv">
                  {classes.classes.map((entry) => (
                    <p key={entry.qualified} style={{ margin: "1px 0" }}>
                      <Link href={`/std/${entry.unit}/${entry.name}/`}>
                        {entry.qualified}
                      </Link>
                    </p>
                  ))}
                </div>
              </td>
            </tr>
            <tr className="row">
              <td colSpan={3}>
                <p>
                  <b>
                    <Link href="/diagnostics/">Diagnostics</Link>
                  </b>
                  {" "}
                  <span className="t-mark-rev">({diagnostics.count} codes)</span>
                </p>
                <div className="mainpagediv">
                  <Run
                    items={families.map((family) => ({
                      label: family,
                      href: `/diagnostics/#${family.toLowerCase().replace(/[^a-z]+/g, "-")}`,
                    }))}
                  />
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </>
  );
}
