import assert from "node:assert/strict";
import test from "node:test";

import {
  codeLanguageFromClassName,
  highlightCode,
} from "../src/highlighting/HighlightedCode";

test("maps MDX fence language classes onto supported reference languages", () => {
  assert.equal(codeLanguageFromClassName("language-gti"), "gti");
  assert.equal(codeLanguageFromClassName("foo language-sh bar"), "bash");
  assert.equal(codeLanguageFromClassName("language-unknown"), "text");
  assert.equal(codeLanguageFromClassName(undefined), "text");
});

test("highlights GTI-specific syntax for both site color schemes", async () => {
  const html = await highlightCode(
    "mut std::vector<int> values = std::vector<int>();",
    "gti",
  );

  assert.match(html, /class="shiki/);
  assert.match(html, /--shiki-dark/);
  assert.match(html, />mut</);
  assert.match(html, />vector</);
});
