import type { Metadata } from "next";
import Link from "next/link";
import "./globals.css";

export const metadata: Metadata = {
  title: {
    default: "GTI Reference",
    template: "%s — GTI Reference",
  },
  description:
    "The reference for the GTI programming language and its standard library.",
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
        <main className="layout">{children}</main>
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
