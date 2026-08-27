import createMDX from "@next/mdx";

const withMDX = createMDX({
  options: {
    // String plugin names keep Turbopack's loader options serializable.
    remarkPlugins: ["remark-frontmatter"],
  },
});

/** @type {import("next").NextConfig} */
const nextConfig = {
  output: "export",
  pageExtensions: ["ts", "tsx", "md", "mdx"],
  trailingSlash: true,
};

export default withMDX(nextConfig);
