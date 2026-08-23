"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";

// The cppreference-style navigation tree, present on every page. Data is
// baked in at build time from the generated indexes via props, so the tree
// can never list a unit or section that does not exist.

export interface SidebarData {
  language: { slug: string; title: string }[];
  units: string[];
  diagnosticFamilies: string[];
}

function shorten(title: string): string {
  // Section titles carry numbering like "7. Standard Library" in some files;
  // the sidebar shows them compactly.
  return title.replace(/^\d+\.\s*/, "");
}

export default function Sidebar({ data }: { data: SidebarData }) {
  const pathname = usePathname();
  const active = (href: string) =>
    pathname === href ? "sidebar-link active" : "sidebar-link";

  return (
    <nav className="sidebar" aria-label="Reference navigation">
      <div className="sidebar-group">
        <Link href="/language/" className="sidebar-heading">
          Language
        </Link>
        {data.language.map((section) => (
          <Link
            key={section.slug}
            href={`/language/${section.slug}/`}
            className={active(`/language/${section.slug}/`)}
          >
            {shorten(section.title)}
          </Link>
        ))}
      </div>
      <div className="sidebar-group">
        <Link href="/std/" className="sidebar-heading">
          Standard library
        </Link>
        {data.units.map((unit) => (
          <Link
            key={unit}
            href={`/std/${unit}/`}
            className={active(`/std/${unit}/`)}
          >
            {`<std/${unit}>`}
          </Link>
        ))}
      </div>
      <div className="sidebar-group">
        <Link href="/diagnostics/" className="sidebar-heading">
          Diagnostics
        </Link>
        {data.diagnosticFamilies.map((family) => (
          <Link
            key={family}
            href={`/diagnostics/#${family.toLowerCase().replace(/[^a-z]+/g, "-")}`}
            className="sidebar-link"
          >
            {family}
          </Link>
        ))}
      </div>
    </nav>
  );
}
