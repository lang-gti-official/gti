"use client";

import Link from "next/link";
import { useSearchParams } from "next/navigation";
import index from "@/generated/search-index.json";

export default function SearchResults() {
  const query = (useSearchParams().get("q") ?? "").trim().toLowerCase();
  if (!query) {
    return <p>Enter a name to search for: a header, class, member, function, language section, or diagnostic code.</p>;
  }

  const scored = index.entries
    .map((entry) => {
      const title = entry.title.toLowerCase();
      let score = 0;
      if (title === query) score = 3;
      else if (title.endsWith(`::${query}`) || title === `<std/${query}>`) score = 2;
      else if (title.includes(query)) score = 1;
      return { entry, score };
    })
    .filter((candidate) => candidate.score > 0)
    .sort((a, b) => b.score - a.score || a.entry.title.localeCompare(b.entry.title))
    .slice(0, 200);

  if (scored.length === 0) {
    return (
      <p>
        No page matches <b>{query}</b>.
      </p>
    );
  }

  return (
    <>
      <p>
        {scored.length} result{scored.length === 1 ? "" : "s"} for <b>{query}</b>
      </p>
      <table className="t-dsc-begin">
        <tbody>
          {scored.map(({ entry }) => (
            <tr className="t-dsc" key={entry.href + entry.title}>
              <td>
                <div className="t-dsc-member-div">
                  <div>
                    <Link href={("anchor" in entry && entry.anchor) ? `${entry.href}#${entry.anchor}` : entry.href}>
                      <span className="t-lines">
                        <span>{entry.title}</span>
                      </span>
                    </Link>
                  </div>
                </div>
              </td>
              <td>
                <span className="t-mark">({entry.kind})</span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </>
  );
}
