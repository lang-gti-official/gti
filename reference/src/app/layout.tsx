import type { Metadata } from "next";
import Link from "next/link";
import type { ReactNode } from "react";

import { ReferenceSearch } from "@/components/ReferenceSearch";
import type { ReferenceNavigationEntry } from "@/content/types";
import { referencePages } from "@/generated/reference-pages";

import "./globals.css";

export const metadata: Metadata = {
  title: {
    default: "GTI reference",
    template: "%s — GTI reference",
  },
  description: "Language and standard-library reference for GTI.",
};

const navigationEntries: ReferenceNavigationEntry[] = referencePages.map(
  (page) => ({
    route: page.route,
    title: page.metadata.title,
    description: page.metadata.description,
    kind: page.metadata.kind,
  }),
);

const sidebarEntries = navigationEntries.filter(
  (entry) => entry.kind !== "member",
);

function NavigationSection({
  title,
  entries,
}: {
  title: string;
  entries: ReferenceNavigationEntry[];
}) {
  if (entries.length === 0) {
    return null;
  }
  return (
    <section>
      <h2>{title}</h2>
      <ul>
        {entries.map((entry) => (
          <li key={entry.route}>
            <Link href={`/${entry.route}/`}>{entry.title}</Link>
          </li>
        ))}
      </ul>
    </section>
  );
}

export default function RootLayout({ children }: { children: ReactNode }) {
  const languageEntries = sidebarEntries.filter(
    (entry) => entry.route !== "std" && !entry.route.startsWith("std/"),
  );
  const libraryEntries = sidebarEntries.filter((entry) =>
    entry.route === "std" || entry.route.startsWith("std/"),
  );
  return (
    <html lang="en">
      <body>
        <header className="site-header">
          <div className="header-inner">
            <Link className="wordmark" href="/" aria-label="GTI reference home">
              <span>GTI</span>
              <small>reference</small>
            </Link>
            <nav className="primary-navigation" aria-label="Primary">
              <Link href="/language/">Language</Link>
              <Link href="/std/">Standard library</Link>
            </nav>
            <ReferenceSearch entries={navigationEntries} />
          </div>
        </header>
        <div className="site-layout">
          <aside className="reference-navigation" aria-label="Reference pages">
            <p className="navigation-title">Contents</p>
            <NavigationSection title="Language" entries={languageEntries} />
            <NavigationSection
              title="Standard library"
              entries={libraryEntries}
            />
          </aside>
          <main>{children}</main>
        </div>
        <footer className="site-footer">
          <p>
            GTI reference pages combine authored guidance with declarations
            projected directly from the compiler.
          </p>
        </footer>
      </body>
    </html>
  );
}
