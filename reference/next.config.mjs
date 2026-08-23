import createMDX from "@next/mdx";

/** @type {import('next').NextConfig} */
const nextConfig = {
  // The reference publishes as static HTML: `next dev` for local reading,
  // `next build` emits `out/` for any static host. No server to operate.
  output: "export",
  // Directory-style URLs (`/std/filesystem/`) so anchors stay stable when
  // hosted from a plain file server or GitHub Pages.
  trailingSlash: true,
  pageExtensions: ["ts", "tsx", "mdx"],
};

const withMDX = createMDX({});

export default withMDX(nextConfig);
