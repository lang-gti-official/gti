import type { Metadata } from "next";
import Link from "next/link";
import Sidebar from "@/components/Sidebar";
import language from "@/generated/language.json";
import units from "@/generated/units.json";
import diagnostics from "@/generated/diagnostics.json";
import "./globals.css";

export const metadata: Metadata = {
  title: {
    default: "GTI Reference",
    template: "%s — GTI Reference",
  },
  description:
    "The reference for the GTI programming language and its standard library.",
};

const sidebarData = {
  language: language.sections.map((section) => ({
    slug: section.slug,
    title: section.title,
  })),
  units: units.units.map((unit) => unit.name),
  diagnosticFamilies: [
    ...new Set(diagnostics.diagnostics.map((d) => d.family)),
  ],
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>
        <header className="site-header">
          <div className="site-header-inner">
            <Link href="/" className="site-title">
              <span>GTI</span> Reference
            </Link>
            <nav className="site-nav">
              <Link href="/language/">Language</Link>
              <Link href="/std/">Standard library</Link>
              <Link href="/diagnostics/">Diagnostics</Link>
              <Link href="/about/">About</Link>
            </nav>
          </div>
        </header>
        <div className="shell">
          <Sidebar data={sidebarData} />
          <main className="content">{children}</main>
        </div>
        <footer className="site-footer">
          <div className="site-footer-inner">
            <span>
              Facts on this site are generated from the GTI repository at build
              time; prose is authored alongside them.
            </span>
            <span>
              Normative source: <code>docs/language/</code>
            </span>
          </div>
        </footer>
      </body>
    </html>
  );
}
