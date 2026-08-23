import type { Metadata } from "next";
import Link from "next/link";
import language from "@/generated/language.json";

export const metadata: Metadata = {
  title: "Language",
  description: "The GTI language specification sections.",
};

export default function LanguageIndex() {
  return (
    <>
      <h1>Language</h1>
      <p className="lede">
        The language pages present the GTI specification: lexical structure,
        static semantics, ownership and lifetimes, execution, and target
        behaviour. The normative source lives in{" "}
        <code>docs/language/</code>; this index is generated from it so the
        two can never list different sections.
      </p>

      <table className="reference-table">
        <thead>
          <tr>
            <th>Section</th>
            <th>Status</th>
            <th>Source</th>
          </tr>
        </thead>
        <tbody>
          {language.sections.map((section) => (
            <tr key={section.slug} id={section.slug}>
              <td>
                <Link href={`/language/${section.slug}/`}>{section.title}</Link>
              </td>
              <td>
                {section.status ? (
                  <span className="badge">{section.status}</span>
                ) : (
                  <span className="muted">—</span>
                )}
              </td>
              <td>
                <code>docs/language/{section.file}</code>
              </td>
            </tr>
          ))}
        </tbody>
      </table>

      <div className="stub-notice">
        Each section renders the normative markdown captured at build time —
        quoted, never paraphrased.
      </div>
    </>
  );
}
