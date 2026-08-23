import { Marked, type Tokens } from "marked";
import { highlight } from "./highlight";

// Renders a docs/language/*.md source to HTML at build time.
//
// The markdown itself is the normative text and is quoted, not transformed:
// this renderer only adds stable heading anchors, syntax highlighting for
// fenced code, and rewrites repository links so navigation keeps working
// when the page is served from the site instead of from a file tree.

const REPOSITORY = "https://github.com/LukeHHA/gti/blob/main";

function slugify(text: string): string {
  return text
    .toLowerCase()
    .replace(/<[^>]+>/g, "")
    .replace(/[^a-z0-9\s.-]/g, "")
    .replace(/[\s.]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "");
}

function escapeHtml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

export async function renderMarkdown(source: string): Promise<string> {
  // Fenced blocks are highlighted asynchronously before rendering; the
  // renderer then emits the precomputed HTML. This is the documented marked
  // pattern for async work: walkTokens completes first.
  const highlighted = new Map<string, string>();

  const marked = new Marked({
    gfm: true,
    async: true,
    walkTokens: async (token) => {
      if (token.type !== "code") return;
      const code = token as Tokens.Code;
      const html = await highlight(code.text, code.lang || undefined);
      if (html) {
        highlighted.set(code.text, html);
      }
    },
  });

  marked.use({
    renderer: {
      heading({ tokens, depth }: Tokens.Heading) {
        const text = this.parser.parseInline(tokens);
        const id = slugify(text);
        return `<h${depth} id="${id}"><a href="#${id}" class="anchor">${text}</a></h${depth}>\n`;
      },
      code({ text }: Tokens.Code) {
        const html = highlighted.get(text);
        if (html) return html;
        return `<pre><code>${escapeHtml(text)}</code></pre>\n`;
      },
      link({ href, tokens }: Tokens.Link) {
        const text = this.parser.parseInline(tokens);
        let target = href;
        if (/^[^/:]+\.md(#.*)?$/.test(href)) {
          // A sibling language section: keep it on the site.
          target = "/language/" + href.replace(/\.md(#.*)?$/, "/$1").replace("/#", "/#");
          target = target.replace(/\.md/, "");
        } else if (href.startsWith("../")) {
          // Elsewhere in the repository: link to the source of record.
          target = `${REPOSITORY}/docs/${href.replace(/^\.\.\//, "")}`;
        }
        return `<a href="${target}">${text}</a>`;
      },
    },
  });

  return marked.parse(source);
}
