import type { Metadata } from "next";
import type { ReactNode } from "react";

import "./globals.css";

export const metadata: Metadata = {
  title: {
    default: "GTI reference",
    template: "%s — GTI reference",
  },
  description: "Language and standard-library reference for GTI.",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en">
      <body>
        <header className="site-header">
          <a className="wordmark" href="/">
            GTI reference
          </a>
          <span className="foundation-label">foundation build</span>
        </header>
        <main>{children}</main>
      </body>
    </html>
  );
}
