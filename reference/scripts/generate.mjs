#!/usr/bin/env node

// Extracts the facts the reference renders from the repository itself, so a
// page can never disagree with the compiler or the standard library without
// the next build noticing. This is the seed of the pipeline: today it emits
// the stdlib unit list, the diagnostic-code census, and the language-section
// index. The full entity graph arrives when the compiler grows
// `--emit-api-json`; nothing here parses GTI beyond recognizing comments,
// because the compiler owns the grammar.

import { mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = join(here, "..", "..");
const outputDirectory = join(here, "..", "src", "generated");

mkdirSync(outputDirectory, { recursive: true });

function write(name, value) {
  writeFileSync(
    join(outputDirectory, name),
    JSON.stringify(value, null, 2) + "\n",
    "utf8",
  );
  console.log(`generated ${name}`);
}

// --- Standard-library units ------------------------------------------------

const stdlibDirectory = join(repositoryRoot, "stdlib", "std");
const units = readdirSync(stdlibDirectory)
  .filter((entry) => entry.endsWith(".gti"))
  .sort()
  .map((entry) => {
    const name = entry.replace(/\.gti$/, "");
    const text = readFileSync(join(stdlibDirectory, entry), "utf8");
    const lines = text.split("\n");

    // The leading comment block is the unit's own summary.
    const summary = [];
    for (const line of lines) {
      const match = line.match(/^\/\/ ?(.*)$/);
      if (!match) break;
      summary.push(match[1]);
    }

    // Public type names, recognized purely lexically for the index page.
    const types = [...text.matchAll(/^\s*(?:class|struct) ([a-z_][a-z0-9_]*)/gm)]
      .map((match) => match[1])
      .filter((typeName, index, all) => all.indexOf(typeName) === index);

    return {
      name,
      include: `<std/${name}>`,
      summary: summary.join(" ").trim(),
      types,
      lineCount: lines.length,
    };
  });

write("units.json", { generatedFrom: "stdlib/std", units });

// --- Diagnostic codes --------------------------------------------------------

const families = {
  L: "Lexer",
  P: "Parser",
  I: "Source loading and includes",
  S: "Semantics",
  R: "Runtime failures",
  B: "Backend",
  D: "Driver",
};

const codePattern = /GTI-([A-Z])([0-9]{4})/g;
const codes = new Map();

function scanForCodes(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      scanForCodes(path);
      continue;
    }
    if (!/\.(cpp|h|hpp)$/.test(entry.name)) continue;
    const text = readFileSync(path, "utf8");
    for (const match of text.matchAll(codePattern)) {
      const code = `GTI-${match[1]}${match[2]}`;
      if (!codes.has(code)) {
        codes.set(code, { code, family: families[match[1]] ?? match[1], files: [] });
      }
      const relative = path.slice(repositoryRoot.length + 1);
      const record = codes.get(code);
      if (!record.files.includes(relative)) record.files.push(relative);
    }
  }
}

scanForCodes(join(repositoryRoot, "src"));

const diagnostics = [...codes.values()].sort((a, b) =>
  a.code.localeCompare(b.code),
);

write("diagnostics.json", {
  generatedFrom: "src",
  count: diagnostics.length,
  diagnostics,
});

// --- Language specification sections ----------------------------------------

const languageDirectory = join(repositoryRoot, "docs", "language");
const sections = readdirSync(languageDirectory)
  .filter((entry) => entry.endsWith(".md") && entry !== "index.md")
  .sort()
  .map((entry) => {
    const text = readFileSync(join(languageDirectory, entry), "utf8");
    const heading = text.match(/^# (.+)$/m);
    const status = text.match(/^Status: (.+)$/m);
    return {
      file: entry,
      slug: entry.replace(/\.md$/, ""),
      title: heading ? heading[1] : entry,
      status: status ? status[1] : null,
    };
  });

write("language.json", { generatedFrom: "docs/language", sections });
