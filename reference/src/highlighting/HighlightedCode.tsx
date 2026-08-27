import { createHighlighter } from "shiki";

import { gtiLanguage } from "./gti-language";

export type ReferenceCodeLanguage = "gti" | "bash" | "toml" | "json" | "text";

const languageAliases: Readonly<Record<string, ReferenceCodeLanguage>> = {
  bash: "bash",
  console: "bash",
  gti: "gti",
  json: "json",
  sh: "bash",
  shell: "bash",
  text: "text",
  plaintext: "text",
  toml: "toml",
  txt: "text",
};

const highlighter = createHighlighter({
  langs: ["bash", "cpp", "json", "toml", gtiLanguage],
  themes: ["github-light", "github-dark"],
});

export function codeLanguageFromClassName(
  className: string | undefined,
): ReferenceCodeLanguage {
  const declared = className?.match(/(?:^|\s)language-([^\s]+)/)?.[1];
  return languageAliases[declared ?? ""] ?? "text";
}

export async function highlightCode(
  code: string,
  language: ReferenceCodeLanguage,
): Promise<string> {
  const loaded = await highlighter;
  return loaded.codeToHtml(code, {
    lang: language,
    themes: {
      light: "github-light",
      dark: "github-dark",
    },
    defaultColor: false,
  });
}

export async function HighlightedCode({
  code,
  language,
  className,
}: {
  code: string;
  language: ReferenceCodeLanguage;
  className?: string;
}) {
  const highlighted = await highlightCode(code, language);
  return (
    <div
      className={["syntax-highlight", className].filter(Boolean).join(" ")}
      dangerouslySetInnerHTML={{ __html: highlighted }}
    />
  );
}
