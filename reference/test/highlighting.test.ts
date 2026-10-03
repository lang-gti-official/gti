import assert from "node:assert/strict";
import test from "node:test";

import {
  codeLanguageFromClassName,
  highlightCode,
} from "../src/highlighting/HighlightedCode";
import { gtiLanguage } from "../src/highlighting/gti-language";

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

test("recognizes module interface keywords without matching identifier prefixes", () => {
  for (const [keyword, scope] of [
    ["module", "storage.type.gti"],
    ["export", "storage.modifier.gti"],
    ["import", "keyword.control.gti"],
  ]) {
    const rule = gtiLanguage.patterns?.find(
      (pattern) => pattern.name === scope && pattern.match,
    );
    assert.ok(rule?.match);
    const expression = new RegExp(rule.match);
    assert.match(keyword, expression);
    assert.doesNotMatch(`${keyword}_name`, expression);
  }
});
