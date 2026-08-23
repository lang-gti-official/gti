import type { Metadata } from "next";
import Link from "next/link";
import Tabs from "@/components/Tabs";
import version from "@/generated/version.json";
import "./globals.css";

export const metadata: Metadata = {
  title: {
    default: "GTI Reference",
    template: "%s - gtireference",
  },
  description:
    "The reference for the GTI programming language and its standard library.",
};

// The Cppreference2 skin skeleton, by id, so the structure is recognizable
// to anyone who has read cppreference: #mw-head (site name, search, personal
// links; then tabs), #cpp-content-base > #content, #cpp-footer-base.

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>
        <div id="mw-head">
          <div id="cpp-head-first-base">
            <div id="cpp-head-first">
              <h5>
                <Link href="/">gtireference</Link>
              </h5>
              <div id="cpp-head-search">
                <form action="/search/" id="searchform">
                  <div id="simpleSearch">
                    <input
                      type="search"
                      name="q"
                      placeholder="Search gtireference"
                      aria-label="Search gtireference"
                      id="searchInput"
                    />
                    <button type="submit" title="Search">
                      🔍
                    </button>
                  </div>
                </form>
              </div>
              <div id="cpp-head-personal">
                <a href="https://github.com/LukeHHA/gti">Repository</a>
                <a href="https://github.com/LukeHHA/gti/releases">Releases</a>
              </div>
            </div>
          </div>
          <Tabs />
        </div>
        <div id="cpp-content-base">
          <div id="content">{children}</div>
        </div>
        <div id="cpp-footer-base">
          <div id="footer">
            <div id="cpp-navigation">
              <h5>Navigation</h5>
              <ul>
                <li>
                  <Link href="/">Main page</Link>
                </li>
                <li>
                  <Link href="/language/">Language</Link>
                </li>
                <li>
                  <Link href="/std/">Standard library headers</Link>
                </li>
                <li>
                  <Link href="/diagnostics/">Diagnostics</Link>
                </li>
                <li>
                  <Link href="/about/">About this reference</Link>
                </li>
              </ul>
            </div>
            <div id="cpp-toolbox">
              <h5>Toolbox</h5>
              <ul>
                <li>
                  <a href="https://github.com/LukeHHA/gti/tree/main/reference">
                    Site source
                  </a>
                </li>
                <li>
                  <a href="https://github.com/LukeHHA/gti/issues/new">
                    Report a problem
                  </a>
                </li>
              </ul>
            </div>
            <div id="footer-info">
              Built from repository commit {version.commit}; every example was
              compiled and executed by {version.gtiVersion} at build time.
            </div>
          </div>
        </div>
      </body>
    </html>
  );
}
