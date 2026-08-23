import Link from "next/link";
import Navbar from "@/components/Navbar";
import Code from "@/components/Code";

// The cppreference page anatomy as components, named after the wiki
// template classes they reproduce (t-dcl, t-dsc, t-example) so the rendered
// DOM reads like the original.

export function Page({
  title,
  children,
}: {
  title: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <>
      <h1 id="firstHeading">{title}</h1>
      <div id="mw-content-text">
        <Navbar />
        {children}
      </div>
    </>
  );
}

/** t-dcl-begin: "Defined in header" row, then numbered declaration rows. */
export function Declarations({
  header,
  declarations,
}: {
  header?: { label: string; href: string };
  declarations: { code: string; mark?: string }[];
}) {
  const numbered = declarations.length > 1;
  return (
    <table className="t-dcl-begin">
      <tbody>
        {header ? (
          <tr className="t-dsc-header">
            <td>
              <div>
                Defined in header{" "}
                <code>
                  <Link href={header.href}>{header.label}</Link>
                </code>
              </div>
            </td>
            <td></td>
            <td></td>
          </tr>
        ) : null}
        {declarations.map((declaration, index) => (
          <tr className="t-dcl" key={declaration.code + index}>
            <td>
              <Code code={declaration.code} lang="gti" />
            </td>
            <td>{numbered ? `(${index + 1})` : ""}</td>
            <td>
              {declaration.mark ? (
                <span className="t-mark-rev">{declaration.mark}</span>
              ) : null}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export interface DscRow {
  name: React.ReactNode;
  href?: string;
  anchor?: string;
  description: React.ReactNode;
  kind: string;
}

/** t-dsc-begin: ruled description rows, optionally under hitem group labels. */
export function Descriptions({
  groups,
}: {
  groups: { heading?: string; rows: DscRow[] }[];
}) {
  return (
    <table className="t-dsc-begin">
      <tbody>
        {groups.map((group, groupIndex) => (
          <RowGroup key={group.heading ?? groupIndex} group={group} />
        ))}
      </tbody>
    </table>
  );
}

function RowGroup({ group }: { group: { heading?: string; rows: DscRow[] } }) {
  return (
    <>
      {group.heading ? (
        <tr className="t-dsc-hitem">
          <td colSpan={2}>{group.heading}</td>
        </tr>
      ) : null}
      {group.rows.map((row, index) => (
        <tr className="t-dsc" key={index} id={row.anchor}>
          <td>
            <div className="t-dsc-member-div">
              <div>
                {row.href ? (
                  <Link href={row.href}>
                    <span className="t-lines">
                      <span>{row.name}</span>
                    </span>
                  </Link>
                ) : (
                  <span className="t-lines">
                    <span>{row.name}</span>
                  </span>
                )}
              </div>
            </div>
          </td>
          <td>
            {row.description} <br /> <span className="t-mark">({row.kind})</span>
          </td>
        </tr>
      ))}
    </>
  );
}

/** t-example: the verified badge, source, then Output. */
export function Example({
  source,
  output,
  verifiedWith,
}: {
  source: string;
  output: string;
  verifiedWith: string;
}) {
  return (
    <div className="t-example">
      <div className="t-example-live-link">
        <span className="coliru-btn" title="Compiled and executed at build time">
          Verified with {verifiedWith}
        </span>
      </div>
      <Code code={source} lang="gti" />
      <p>Output:</p>
      <div className="mw-highlight">
        <pre>{output}</pre>
      </div>
    </div>
  );
}
