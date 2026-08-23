import type { Metadata } from "next";
import Link from "next/link";
import units from "@/generated/units.json";
import classes from "@/generated/classes.json";

export const metadata: Metadata = {
  title: "Standard library",
  description: "The GTI standard library units.",
};

export default function StandardLibraryIndex() {
  return (
    <>
      <h1>Standard library</h1>
      <p className="lede">
        Optional units are loaded with <code>#include &lt;std/name&gt;</code>{" "}
        and are independently parsed GTI source. This index is generated from{" "}
        <code>stdlib/std/</code> at build time; every unit listed here exists
        in the toolchain that shipped alongside this page.
      </p>

      <table className="reference-table">
        <thead>
          <tr>
            <th>Include</th>
            <th>Public types</th>
            <th>Summary</th>
          </tr>
        </thead>
        <tbody>
          {units.units.map((unit) => (
            <tr key={unit.name} id={unit.name}>
              <td>
                <Link href={`/std/${unit.name}/`}>
                  <code>{unit.include}</code>
                </Link>
              </td>
              <td>
                {unit.types.length > 0 ? (
                  unit.types.map((typeName) => {
                    const hasPage = classes.classes.some(
                      (entry) =>
                        entry.unit === unit.name && entry.name === typeName,
                    );
                    return (
                      <span key={typeName}>
                        {hasPage ? (
                          <Link href={`/std/${unit.name}/${typeName}/`}>
                            <code>{typeName}</code>
                          </Link>
                        ) : (
                          <code>{typeName}</code>
                        )}{" "}
                      </span>
                    );
                  })
                ) : (
                  <span className="muted">functions only</span>
                )}
              </td>
              <td className="muted">
                {unit.summary
                  ? unit.summary.slice(0, 160) +
                    (unit.summary.length > 160 ? "…" : "")
                  : "—"}
              </td>
            </tr>
          ))}
        </tbody>
      </table>

      <div className="stub-notice">
        Unit pages carry a synopsis quoted from the shipped source and
        compiler-verified examples. Per-member pages arrive with{" "}
        <code>gti --emit-api-json</code>.
      </div>
    </>
  );
}
