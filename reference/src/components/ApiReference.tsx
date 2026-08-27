import Link from "next/link";

import type {
  CompilerDocumentationDeclaration,
  CompilerDocumentationKind,
} from "@/content/types";
import { HighlightedCode } from "@/highlighting/HighlightedCode";

const primaryKinds = new Set<CompilerDocumentationKind>([
  "class",
  "struct",
  "interface",
  "union",
  "enum",
  "concept",
  "type-alias",
]);

interface DeclarationGroup {
  key: string;
  declarations: CompilerDocumentationDeclaration[];
}

function groupDeclarations(
  declarations: CompilerDocumentationDeclaration[],
): DeclarationGroup[] {
  const groups = new Map<string, CompilerDocumentationDeclaration[]>();
  for (const declaration of declarations) {
    const group = groups.get(declaration.groupKey) ?? [];
    group.push(declaration);
    groups.set(declaration.groupKey, group);
  }
  return [...groups].map(([key, grouped]) => ({
    key,
    declarations: grouped,
  }));
}

function yesNo(value: boolean): string {
  return value ? "yes" : "no";
}

function DocumentationSummary({ text }: { text: string }) {
  return (
    <>
      {text.split(/(`[^`\n]+`)/g).map((part, index) =>
        part.startsWith("`") && part.endsWith("`") ? (
          <code key={`${part}-${index}`}>{part.slice(1, -1)}</code>
        ) : (
          part
        ),
      )}
    </>
  );
}

function DeclarationList({
  groups,
  title,
  detailRoutes,
  currentRoute,
}: {
  groups: DeclarationGroup[];
  title: string;
  detailRoutes: ReadonlyMap<string, string>;
  currentRoute: string;
}) {
  if (groups.length === 0) {
    return null;
  }
  return (
    <section className="api-members" aria-label={title}>
      <h3>{title}</h3>
      <div className="member-list">
        {groups.map(({ key, declarations }) => {
          const first = declarations[0];
          const detailPageRoute = detailRoutes.get(key);
          const detailRoute =
            detailPageRoute === undefined || detailPageRoute === currentRoute
              ? undefined
              : detailPageRoute;
          const documentation = declarations.find(
            (declaration) => declaration.documentationMarkdown !== null,
          )?.documentationMarkdown;
          return (
            <article className="member-row" key={key}>
              <div
                className={`member-name${detailRoute ? " has-detail" : ""}`}
              >
                {detailRoute ? (
                  <Link
                    className="member-detail-link"
                    href={`/${detailRoute}/`}
                    aria-label={`Read the ${first.qualifiedName} reference`}
                  >
                    <code>{first.name}</code>
                    <span>
                      {first.kind.replaceAll("-", " ")} · details →
                    </span>
                  </Link>
                ) : (
                  <>
                    <code>{first.name}</code>
                    <span>{first.kind.replaceAll("-", " ")}</span>
                  </>
                )}
              </div>
              <div className="member-signatures">
                {declarations.map((declaration) => (
                  <HighlightedCode
                    code={declaration.signature ?? declaration.qualifiedName}
                    language="gti"
                    key={declaration.key}
                  />
                ))}
                {documentation ? (
                  <p>
                    <DocumentationSummary text={documentation} />
                  </p>
                ) : null}
              </div>
            </article>
          );
        })}
      </div>
    </section>
  );
}

export function ApiReference({
  declarations,
  detailRoutes,
  currentRoute,
}: {
  declarations: CompilerDocumentationDeclaration[];
  detailRoutes: ReadonlyMap<string, string>;
  currentRoute: string;
}) {
  if (declarations.length === 0) {
    return null;
  }
  const groups = groupDeclarations(declarations);
  const primary = groups.find(({ declarations: grouped }) =>
    primaryKinds.has(grouped[0].kind),
  );
  const remaining = groups.filter((group) => group !== primary);
  const constructors = remaining.filter(
    ({ declarations: grouped }) => grouped[0].kind === "constructor",
  );
  const members = remaining.filter(
    ({ declarations: grouped }) =>
      grouped[0].kind === "method" || grouped[0].kind === "operator",
  );
  const related = remaining.filter(
    (group) => !constructors.includes(group) && !members.includes(group),
  );
  const primaryDeclaration = primary?.declarations[0];
  const traits = primaryDeclaration?.signature?.includes("<")
    ? undefined
    : primaryDeclaration?.traits;

  return (
    <section className="api-reference" aria-labelledby="synopsis-heading">
      <div className="section-heading">
        <h2 id="synopsis-heading">Synopsis</h2>
      </div>
      {primary ? (
        <div className="primary-declaration">
          {primary.declarations.map((declaration) => (
            <HighlightedCode
              code={declaration.signature ?? declaration.qualifiedName}
              language="gti"
              key={declaration.key}
            />
          ))}
          {primaryDeclaration?.documentationMarkdown ? (
            <p>
              <DocumentationSummary
                text={primaryDeclaration.documentationMarkdown}
              />
            </p>
          ) : null}
          {primaryDeclaration ? (
            <span className="source-note">
              &lt;{primaryDeclaration.source.unit}&gt; · line{" "}
              {primaryDeclaration.source.line}
            </span>
          ) : null}
        </div>
      ) : null}
      {traits ? (
        <dl className="trait-grid" aria-label="Semantic type traits">
          <div>
            <dt>Ownership</dt>
            <dd>{traits.ownership}</dd>
          </div>
          <div>
            <dt>Cleanup</dt>
            <dd>{traits.drop}</dd>
          </div>
          <div>
            <dt>Copyable</dt>
            <dd>{yesNo(traits.copyable)}</dd>
          </div>
          <div>
            <dt>Movable</dt>
            <dd>{yesNo(traits.movable)}</dd>
          </div>
        </dl>
      ) : null}
      <DeclarationList
        groups={constructors}
        title="Constructors"
        detailRoutes={detailRoutes}
        currentRoute={currentRoute}
      />
      <DeclarationList
        groups={members}
        title="Member functions"
        detailRoutes={detailRoutes}
        currentRoute={currentRoute}
      />
      <DeclarationList
        groups={related}
        title="Related declarations"
        detailRoutes={detailRoutes}
        currentRoute={currentRoute}
      />
    </section>
  );
}
