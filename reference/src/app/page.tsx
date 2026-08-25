import { referencePages } from "@/generated/reference-pages";

export default function HomePage() {
  return (
    <article className="reference-page">
      <p className="eyebrow">GTI language and library</p>
      <h1>Reference foundation</h1>
      <p>
        The content pipeline is connected to compiler-owned API data. Authored
        reference pages will appear here as they are written and verified.
      </p>
      <dl className="foundation-facts">
        <div>
          <dt>Authored pages</dt>
          <dd>{referencePages.length}</dd>
        </div>
        <div>
          <dt>Content model</dt>
          <dd>Page-owned</dd>
        </div>
        <div>
          <dt>API source</dt>
          <dd>GTI semantics</dd>
        </div>
      </dl>
    </article>
  );
}
