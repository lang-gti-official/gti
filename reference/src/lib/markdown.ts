import { Marked, type Tokens } from "marked";

// Renders a docs/language/*.md source to HTML at build time.
//
// The markdown itself is the normative text and is quoted, not transformed:
// this renderer only adds stable heading anchors and rewrites repository
// links so navigation keeps working when the page is served from the site
// instead of from a file tree.

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

export function renderMarkdown(source: string): string {
  const marked = new Marked({ gfm: true });

  marked.use({
    renderer: {
      heading({ tokens, depth }: Tokens.Heading) {
        const text = this.parser.parseInline(tokens);
        const id = slugify(text);
        return `<h${depth} id="${id}"><a href="#${id}" class="anchor">${text}</a></h${depth}>\n`;
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

  return marked.parse(source, { async: false }) as string;
}
