const { spawnSync } = require("node:child_process");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const executable = path.join(
  root,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "tree-sitter.cmd" : "tree-sitter",
);
const query = path.resolve(root, "..", "queries", "gti", "highlights.scm");
const fixture = path.join(__dirname, "highlights.gti");
const result = spawnSync(executable, ["query", query, fixture, "--captures"], {
  cwd: root,
  encoding: "utf8",
});

if (result.status !== 0) {
  process.stderr.write(result.stderr);
  process.stderr.write(result.stdout);
  process.exit(result.status ?? 1);
}

const captures = [];
const capturePattern =
  /capture:\s+\d+ - ([^,]+), start: \((\d+), (\d+)\), end: .* text: `(.*)`$/;
for (const line of result.stdout.split("\n")) {
  const match = line.match(capturePattern);
  if (match) {
    captures.push({
      name: match[1],
      row: Number(match[2]),
      column: Number(match[3]),
      text: match[4],
    });
  }
}

function requireCapture(row, text, name) {
  if (
    !captures.some(
      (capture) =>
        capture.row === row && capture.text === text && capture.name === name,
    )
  ) {
    throw new Error(
      `Missing @${name} capture for ${JSON.stringify(text)} on row ${row + 1}`,
    );
  }
}

requireCapture(0, "left", "variable.parameter");
requireCapture(1, "if", "keyword.conditional");
requireCapture(1, "left", "variable");
requireCapture(1, "&&", "operator");
requireCapture(1, "||", "operator");
requireCapture(1, "or", "keyword.operator");
requireCapture(1, "and", "keyword.operator");
requireCapture(2, "return", "keyword.return");
requireCapture(3, "else", "keyword.conditional");
requireCapture(8, "...", "punctuation.special");
requireCapture(9, "for", "keyword.repeat");
requireCapture(10, "values", "variable");
requireCapture(0, "::", "punctuation.delimiter");
requireCapture(0, "<", "punctuation.bracket");

process.stdout.write("GTI Tree-sitter highlight captures passed.\n");
