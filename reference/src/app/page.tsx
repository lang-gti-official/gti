import Link from "next/link";

import { referencePages } from "@/generated/reference-pages";

export default function HomePage() {
  const declarationCount = new Set(
    referencePages.flatMap((page) =>
      page.apiDeclarations.map((declaration) => declaration.key),
    ),
  ).size;
  const exampleCount = referencePages.reduce(
    (total, page) => total + page.examples.length,
    0,
  );
  const libraryPages = referencePages.filter((page) =>
    (page.route === "std" || page.route.startsWith("std/")) &&
    page.metadata.kind !== "member",
  );
  return (
    <article className="reference-page home-page">
      <section className="home-hero">
        <div>
          <p className="eyebrow">GTI language and standard library</p>
          <h1>Learn the language. Check the exact API.</h1>
          <p className="summary">
            A practical reference for writing GTI, shaped like the C++
            reference you already know and backed by the declarations the GTI
            compiler actually understands.
          </p>
          <div className="hero-actions">
            <Link className="primary-action" href="/language/">
              Start with the language
            </Link>
            <Link className="secondary-action" href="/std/vector/vector/">
              Explore std::vector
            </Link>
          </div>
        </div>
        <dl className="reference-facts">
          <div>
            <dt>Published pages</dt>
            <dd>{referencePages.length}</dd>
          </div>
          <div>
            <dt>Compiler declarations</dt>
            <dd>{declarationCount}</dd>
          </div>
          <div>
            <dt>Verified examples</dt>
            <dd>{exampleCount}</dd>
          </div>
        </dl>
      </section>
      <section className="home-section" aria-labelledby="browse-heading">
        <div className="section-heading">
          <p className="section-kicker">Reference map</p>
          <h2 id="browse-heading">Browse what is implemented</h2>
        </div>
        <div className="reference-card-grid">
          <Link className="reference-card" href="/language/">
            <span>Language</span>
            <strong>Core GTI concepts</strong>
            <p>Syntax, ownership, programs, build workflow, and interoperation.</p>
          </Link>
          {libraryPages.map((page) => (
            <Link
              className="reference-card"
              href={`/${page.route}/`}
              key={page.route}
            >
              <span>{page.metadata.kind.replaceAll("-", " ")}</span>
              <strong>{page.metadata.title}</strong>
              <p>{page.metadata.description}</p>
            </Link>
          ))}
        </div>
      </section>
    </article>
  );
}
