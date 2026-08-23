import type { Metadata } from "next";
import { Suspense } from "react";
import SearchResults from "./SearchResults";

export const metadata: Metadata = { title: "Search" };

// Special:Search. The index is generated at build time over every page the
// site has; filtering runs in the browser, so the static export needs no
// server.
export default function SearchPage() {
  return (
    <>
      <h1 id="firstHeading">Search results</h1>
      <div id="mw-content-text">
        <Suspense fallback={<p>Searching…</p>}>
          <SearchResults />
        </Suspense>
      </div>
    </>
  );
}
