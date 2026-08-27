import type { ReferenceExample } from "@/content/types";

export function ExampleReference({
  examples,
}: {
  examples: ReferenceExample[];
}) {
  if (examples.length === 0) {
    return null;
  }
  return (
    <section className="examples-reference" aria-labelledby="examples-heading">
      <div className="section-heading">
        <h2 id="examples-heading">
          {examples.length === 1 ? "Example" : "Examples"}
        </h2>
      </div>
      {examples.map((example) => (
        <article className="example-card" key={example.id}>
          <header>
            <div>
              <h3>{example.title}</h3>
              <p>{example.description}</p>
            </div>
          </header>
          <div className="code-panel">
            <div className="code-panel-label">GTI</div>
            <pre>
              <code>{example.sourceText}</code>
            </pre>
          </div>
          {example.mode === "run" && example.expectedStdout !== undefined ? (
            <div className="output-panel">
              <div className="code-panel-label">Output</div>
              <pre>
                <code>{example.expectedStdout}</code>
              </pre>
            </div>
          ) : null}
          {example.mode === "diagnostic" ? (
            <p className="diagnostic-expectation">
              Expected: {example.diagnostics.join(", ")}
            </p>
          ) : null}
        </article>
      ))}
    </section>
  );
}
