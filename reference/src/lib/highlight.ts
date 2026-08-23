import { codeToHtml } from "shiki";

// Build-time syntax highlighting. GTI has no published grammar for
// highlighters yet, and its surface is deliberately C++-familiar, so `gti`
// blocks are highlighted with the C++ grammar — a rendering choice only;
// nothing semantic is claimed by it. Output blocks and unknown languages
// stay plain rather than being dressed up as code.
//
// Shiki runs during static generation, so pages ship highlighted HTML with
// zero client-side JavaScript. Dual themes emit CSS variables; globals.css
// flips them with prefers-color-scheme.

const LANGUAGE_BY_FENCE: Record<string, string> = {
  gti: "cpp",
  cpp: "cpp",
  c: "c",
  sh: "shellscript",
  bash: "shellscript",
  toml: "toml",
  json: "json",
};

export async function highlight(
  code: string,
  fence: string | undefined,
): Promise<string | null> {
  const language = fence ? LANGUAGE_BY_FENCE[fence] : undefined;
  if (!language) {
    return null;
  }
  return codeToHtml(code.replace(/\n$/, ""), {
    lang: language,
    themes: { light: "github-light", dark: "github-dark" },
    defaultColor: false,
  });
}
