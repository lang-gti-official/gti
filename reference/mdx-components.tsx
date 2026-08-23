import type { MDXComponents } from "mdx/types";

// Shared component map for every MDX page. Reference components such as
// <Diagnostic code="…"/> and <Grammar rule="…"/> register here as they are
// built, so prose pages gain them without per-file imports.
export function useMDXComponents(components: MDXComponents): MDXComponents {
  return {
    ...components,
  };
}
