const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const grammarRoot = path.resolve(__dirname, "..");
const repositoryRoot = path.resolve(grammarRoot, "..");
const executable = path.join(
  grammarRoot,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "tree-sitter.cmd" : "tree-sitter",
);

function collectGtiSources(directory) {
  const sources = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const entryPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      sources.push(...collectGtiSources(entryPath));
    } else if (entry.isFile() && entry.name.endsWith(".gti")) {
      sources.push(entryPath);
    }
  }
  return sources;
}

const sources = ["examples", "stdlib"]
  .flatMap((directory) =>
    collectGtiSources(path.join(repositoryRoot, directory)),
  )
  .sort();

if (sources.length === 0) {
  throw new Error("No shipped GTI sources were found to parse");
}

const result = spawnSync(executable, ["parse", "--quiet", ...sources], {
  cwd: grammarRoot,
  encoding: "utf8",
});
const output = `${result.stdout ?? ""}${result.stderr ?? ""}`;

if (
  result.error ||
  result.status !== 0 ||
  /\b(?:ERROR|MISSING)\b/.test(output)
) {
  process.stderr.write(output);
  throw result.error ?? new Error("A shipped GTI source contains parse errors");
}

process.stdout.write(
  `GTI Tree-sitter parsed ${sources.length} shipped sources without errors.\n`,
);
