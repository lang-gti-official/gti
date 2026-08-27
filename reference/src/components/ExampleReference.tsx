import type { ReferenceExample } from "@/content/types";

function modeLabel(example: ReferenceExample): string {
  switch (example.mode) {
    case "compile":
      return "compile checked";
    case "run":
      return "compiled and run";
    case "diagnostic":
      return "diagnostic checked";
  }
}

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
        <p className="section-kicker">Page-owned programs</p>
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
            <span className="verification-badge">{modeLabel(example)}</span>
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
