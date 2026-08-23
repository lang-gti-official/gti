import Link from "next/link";
import units from "@/generated/units.json";
import diagnostics from "@/generated/diagnostics.json";
import language from "@/generated/language.json";

export default function Home() {
  return (
    <>
      <h1>GTI Reference</h1>
      <p className="lede">
        The reference for the GTI programming language: a C++-familiar systems
        language with explicit ownership, checked-by-default arithmetic, and
        defined runtime failure. Entity facts here are extracted from the
        compiler and standard library at build time, so a page cannot disagree
        with the implementation without the build noticing.
      </p>

      <ul className="card-grid">
        <li className="card">
          <h3>
            <Link href="/language/">Language</Link>
          </h3>
          <p>
            {language.sections.length} specification sections — lexical
            structure, ownership and lifetimes, execution, static semantics,
            and target behaviour.
          </p>
        </li>
        <li className="card">
          <h3>
            <Link href="/std/">Standard library</Link>
          </h3>
          <p>
            {units.units.length} optional units from{" "}
            <code>&lt;std/array&gt;</code> to <code>&lt;std/vector&gt;</code>,
            plus the prelude.
          </p>
        </li>
        <li className="card">
          <h3>
            <Link href="/diagnostics/">Diagnostics</Link>
          </h3>
          <p>
            {diagnostics.count} stable diagnostic codes across lexing, parsing,
            semantics, runtime failure, and the backend.
          </p>
        </li>
        <li className="card">
          <h3>
            <Link href="/about/">About this reference</Link>
          </h3>
          <p>
            How the site is built, what is generated versus authored, and how
            to contribute a page.
          </p>
        </li>
      </ul>

      <div className="stub-notice">
        Scaffold status: structure and generated indexes are live; per-entity
        pages and authored prose land next. The layout under{" "}
        <code>reference/</code> is ready to populate.
      </div>
    </>
  );
}
