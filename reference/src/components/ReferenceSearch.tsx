"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useId, useMemo, useState } from "react";

import type { ReferenceNavigationEntry } from "@/content/types";

export function ReferenceSearch({
  entries,
}: {
  entries: ReferenceNavigationEntry[];
}) {
  const router = useRouter();
  const resultsId = useId();
  const [query, setQuery] = useState("");
  const normalized = query.trim().toLocaleLowerCase();
  const matches = useMemo(() => {
    if (normalized.length < 2) {
      return [];
    }
    return entries
      .filter((entry) =>
        `${entry.title} ${entry.route} ${entry.description}`
          .toLocaleLowerCase()
          .includes(normalized),
      )
      .slice(0, 7);
  }, [entries, normalized]);
  const showResults = normalized.length >= 2;

  return (
    <form
      className="reference-search"
      role="search"
      onSubmit={(event) => {
        event.preventDefault();
        if (matches[0]) {
          router.push(`/${matches[0].route}/`);
          setQuery("");
        }
      }}
    >
      <label htmlFor={`${resultsId}-input`}>Search the GTI reference</label>
      <div className="search-control">
        <input
          id={`${resultsId}-input`}
          type="search"
          value={query}
          placeholder="Search the reference"
          autoComplete="off"
          onChange={(event) => setQuery(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Escape") {
              setQuery("");
            }
          }}
        />
        <button type="submit">Search</button>
      </div>
      {showResults ? (
        <div className="search-results" id={resultsId} aria-live="polite">
          {matches.length > 0 ? (
            <ul>
              {matches.map((entry) => (
                <li key={entry.route}>
                  <Link
                    href={`/${entry.route}/`}
                    onClick={() => setQuery("")}
                  >
                    <strong>{entry.title}</strong>
                    <span>{entry.description}</span>
                  </Link>
                </li>
              ))}
            </ul>
          ) : (
            <p>No matching reference pages yet.</p>
          )}
        </div>
      ) : null}
    </form>
  );
}
