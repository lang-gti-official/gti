import Link from "next/link";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";
import diagnostics from "@/generated/diagnostics.json";
import language from "@/generated/language.json";
import version from "@/generated/version.json";

// The front page is a dense index in the cppreference tradition: the whole
// surface, grouped, on one screen — not a marketing page.

const UNIT_GROUPS: { title: string; units: string[] }[] = [
  {
    title: "Containers and views",
    units: ["array", "vector", "list", "forward_list", "span"],
  },
  { title: "Strings and formatting", units: ["string", "format"] },
  { title: "Numerics and algorithms", units: ["numeric", "algorithm", "cmath", "random"] },
  { title: "Utilities", units: ["utility", "optional", "functional", "iterator", "memory"] },
  { title: "Input/output and system", units: ["cstdio", "filesystem", "tcp"] },
];

export default function Home() {
  const known = new Set(UNIT_GROUPS.flatMap((group) => group.units));
  const ungrouped = units.units
    .map((unit) => unit.name)
    .filter((name) => !known.has(name));

  return (
    <>
      <h1>GTI Reference</h1>
      <p className="lede">
        GTI is a C++-familiar systems language with explicit ownership,
        checked-by-default arithmetic, and defined runtime failure. This
        reference is built for readers coming from C++: the layout follows
        cppreference, and pages call out exactly where GTI differs.
      </p>

      <div className="index-columns">
        <section>
          <h2>Language</h2>
          <ul className="index-list">
            {language.sections.map((section) => (
              <li key={section.slug}>
                <Link href={`/language/${section.slug}/`}>
                  {section.title.replace(/^\d+\.\s*/, "")}
                </Link>
              </li>
            ))}
          </ul>
        </section>

        <section>
          <h2>Standard library</h2>
          {UNIT_GROUPS.map((group) => (
            <div key={group.title}>
              <h3 className="index-subhead">{group.title}</h3>
              <ul className="index-list">
                {group.units
                  .filter((name) => units.units.some((u) => u.name === name))
                  .map((name) => {
                    const classNames = classes.classes
                      .filter((entry) => entry.unit === name)
                      .map((entry) => entry.name);
                    return (
                      <li key={name}>
                        <Link href={`/std/${name}/`}>
                          <code>&lt;std/{name}&gt;</code>
                        </Link>
                        {classNames.length > 0 ? (
                          <span className="muted">
                            {" — "}
                            {classNames.map((cls, index) => (
                              <span key={cls}>
                                {index > 0 ? ", " : ""}
                                <Link href={`/std/${name}/${cls}/`}>
                                  <code>{cls}</code>
                                </Link>
                              </span>
                            ))}
                          </span>
                        ) : null}
                      </li>
                    );
                  })}
              </ul>
            </div>
          ))}
          {ungrouped.length > 0 ? (
            <div>
              <h3 className="index-subhead">Other</h3>
              <ul className="index-list">
                {ungrouped.map((name) => (
                  <li key={name}>
                    <Link href={`/std/${name}/`}>
                      <code>&lt;std/{name}&gt;</code>
                    </Link>
                  </li>
                ))}
              </ul>
            </div>
          ) : null}
        </section>

        <section>
          <h2>Diagnostics</h2>
          <ul className="index-list">
            <li>
              <Link href="/diagnostics/">
                All {diagnostics.count} stable codes
              </Link>
            </li>
          </ul>
          <h2>About</h2>
          <ul className="index-list">
            <li>
              <Link href="/about/">How this reference is built</Link>
            </li>
          </ul>
        </section>
      </div>

      <div className="stub-notice">
        Every example on this site was compiled and executed by{" "}
        <strong>{version.gtiVersion}</strong> (commit{" "}
        <code>{version.commit}</code>) when the site was built; every
        diagnostic reproducer was verified to fail with exactly the code its
        page documents. A stale page fails the build instead of shipping.
      </div>
    </>
  );
}
