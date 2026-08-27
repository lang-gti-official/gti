import {
  isValidElement,
  type ComponentPropsWithoutRef,
  type ReactNode,
} from "react";
import type { MDXComponents } from "mdx/types";

import {
  codeLanguageFromClassName,
  HighlightedCode,
} from "@/highlighting/HighlightedCode";

function codeText(node: ReactNode): string | undefined {
  if (typeof node === "string" || typeof node === "number") {
    return String(node);
  }
  if (Array.isArray(node)) {
    const parts = node.map(codeText);
    return parts.every((part) => part !== undefined)
      ? parts.join("")
      : undefined;
  }
  return undefined;
}

function MdxPre({
  children,
  ...properties
}: ComponentPropsWithoutRef<"pre">) {
  if (isValidElement<{ className?: string; children?: ReactNode }>(children)) {
    const source = codeText(children.props.children);
    if (source !== undefined) {
      return (
        <HighlightedCode
          code={source.replace(/\n$/, "")}
          language={codeLanguageFromClassName(children.props.className)}
          className="mdx-code-block"
        />
      );
    }
  }
  return <pre {...properties}>{children}</pre>;
}

export function useMDXComponents(components: MDXComponents): MDXComponents {
  return {
    pre: MdxPre,
    ...components,
  };
}
