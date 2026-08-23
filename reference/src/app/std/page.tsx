import type { Metadata } from "next";
import units from "@/generated/units.json";

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
                <code>{unit.include}</code>
              </td>
              <td>
                {unit.types.length > 0 ? (
                  unit.types.map((typeName) => (
                    <span key={typeName}>
                      <code>{typeName}</code>{" "}
                    </span>
                  ))
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
        Per-entity pages (one page per class and member, cppreference-style)
        are the next population step; they render from the compiler&apos;s
        entity graph once <code>gti --emit-api-json</code> lands.
      </div>
    </>
  );
}
