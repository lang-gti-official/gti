"use client";

import { usePathname } from "next/navigation";

// The Cppreference2 tab row: Page | Discussion on the left, View | View
// source | History on the right. On cppreference those are MediaWiki
// actions; here "Discussion" is the repository's issue tracker and "View
// source" / "History" open the file the page was generated from on GitHub,
// so the tabs do what a reader expects rather than being decoration.

const REPO = "https://github.com/LukeHHA/gti";

function sourceFor(pathname: string): string | null {
  const language = pathname.match(/^\/language\/([^/]+)\/$/);
  if (language) return `docs/language/${language[1]}.md`;
  const unit = pathname.match(/^\/std\/([^/]+)\//);
  if (unit) return `stdlib/std/${unit[1]}.gti`;
  if (pathname.startsWith("/diagnostics/")) return "src/compiler";
  return "reference";
}

export default function Tabs() {
  const pathname = usePathname() ?? "/";
  const source = sourceFor(pathname);
  return (
    <div id="cpp-head-second">
      <div id="cpp-head-tools-left">
        <div className="vectorTabs">
          <h5>Namespaces</h5>
          <ul>
            <li className="selected">
              <a href={pathname}>
                <span>Page</span>
              </a>
            </li>
            <li>
              <a href={`${REPO}/issues`}>
                <span>Discussion</span>
              </a>
            </li>
          </ul>
        </div>
      </div>
      <div id="cpp-head-tools-right">
        <div className="stdrev-select">Built from the repository at every run</div>
        <div className="vectorTabs">
          <h5>Views</h5>
          <ul>
            <li className="selected">
              <a href={pathname}>
                <span>View</span>
              </a>
            </li>
            <li>
              <a href={source ? `${REPO}/blob/main/${source}` : REPO}>
                <span>View source</span>
              </a>
            </li>
            <li>
              <a href={source ? `${REPO}/commits/main/${source}` : `${REPO}/commits/main`}>
                <span>History</span>
              </a>
            </li>
          </ul>
        </div>
      </div>
    </div>
  );
}
