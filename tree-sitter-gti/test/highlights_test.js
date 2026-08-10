const { spawnSync } = require("node:child_process");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const executable = path.join(
  root,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "tree-sitter.cmd" : "tree-sitter",
);
const fixture = path.join(__dirname, "highlights.gti");
const capturePattern =
  /capture:\s+\d+ - ([^,]+), start: \((\d+), (\d+)\), end: .* text: `(.*)`$/;

function queryCaptures(name) {
  const query = path.resolve(root, "..", "queries", "gti", `${name}.scm`);
  const result = spawnSync(
    executable,
    ["query", query, fixture, "--captures"],
    { cwd: root, encoding: "utf8" },
  );

  if (result.status !== 0) {
    process.stderr.write(result.stderr);
    process.stderr.write(result.stdout);
    process.exit(result.status ?? 1);
  }

  const captures = [];
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
  return captures;
}

const captures = queryCaptures("highlights");

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
requireCapture(14, "State", "type.definition");
requireCapture(15, "idle", "constant");
requireCapture(16, "running", "constant");
requireCapture(19, "State", "type");
requireCapture(22, "switch", "keyword.conditional");
requireCapture(23, "case", "keyword.conditional");
requireCapture(25, "default", "keyword.conditional");
requireCapture(33, "this", "variable.builtin");
requireCapture(33, "self", "variable");
requireCapture(36, "Counter", "type");
requireCapture(36, "counter", "variable");
requireCapture(38, "static", "keyword.modifier");
requireCapture(38, "file_value", "variable");
requireCapture(42, "static", "keyword.modifier");
requireCapture(42, "count", "variable.member");
requireCapture(43, "static", "keyword.modifier");
requireCapture(43, "current", "function.method");
requireCapture(46, "0x20", "number");
requireCapture(46, "0b10", "number");
requireCapture(46, "*", "operator");
requireCapture(48, "uint64_t", "type.builtin");
requireCapture(48, "N", "variable.parameter");
requireCapture(49, "uint64", "type.builtin");
requireCapture(49, "N", "variable.parameter");
requireCapture(51, "interface", "keyword.type");
requireCapture(51, "Renderable", "type.definition");
requireCapture(52, "render", "function.method");
requireCapture(58, "virtual", "keyword.modifier");
requireCapture(61, "Base", "type");
requireCapture(61, "Renderable", "type");
requireCapture(63, "Base", "variable.member");
requireCapture(64, "override", "keyword.modifier");
requireCapture(65, "override", "keyword.modifier");
requireCapture(72, "++", "operator");
requireCapture(84, "for", "keyword.repeat");
requireCapture(84, "value", "variable");
requireCapture(92, "LifecycleValue", "constructor");
requireCapture(92, "&", "operator");
requireCapture(92, "default", "keyword.modifier");
requireCapture(93, "&&", "operator");
requireCapture(93, "delete", "keyword.modifier");
requireCapture(97, "#error", "keyword.directive");
requireCapture(100, "#include", "keyword.import");
requireCapture(103, "first", "variable");
requireCapture(103, "second", "variable");
requireCapture(107, "concept", "keyword.type");
requireCapture(107, "sortable", "type.definition");
requireCapture(107, "T", "type.parameter");
requireCapture(107, "totally_ordered", "type");
requireCapture(107, "&&", "operator");
requireCapture(107, "movable", "type");
requireCapture(109, "sortable", "type");
requireCapture(114, "do", "keyword.repeat");
requireCapture(116, "while", "keyword.repeat");
requireCapture(120, "?", "operator");
requireCapture(120, ":", "operator");
requireCapture(120, "condition", "variable");
requireCapture(124, "*=", "operator");
requireCapture(125, "/=", "operator");
requireCapture(126, "%=", "operator");
requireCapture(127, "&=", "operator");
requireCapture(128, "|=", "operator");
requireCapture(129, "^=", "operator");
requireCapture(130, "<<=", "operator");
requireCapture(131, ">>=", "operator");
requireCapture(134, "extern", "keyword.modifier");
requireCapture(134, '"C"', "string");
requireCapture(135, "native_close", "function");
requireCapture(135, "descriptor", "variable.parameter");
requireCapture(139, "native_close", "function.call");
requireCapture(142, "raw", "function");
requireCapture(142, "*", "operator");
requireCapture(142, "writable", "variable.parameter");
requireCapture(142, "const", "keyword.modifier");
requireCapture(142, "readable", "variable.parameter");
requireCapture(143, "unsafe", "keyword");
requireCapture(144, "alias", "variable");
requireCapture(144, "&", "operator");
requireCapture(145, "*", "operator");
requireCapture(146, "->", "operator");
requireCapture(146, "value", "variable.member");

const localCaptures = queryCaptures("locals");
function requireLocalCapture(row, text, name) {
  if (
    !localCaptures.some(
      (capture) =>
        capture.row === row && capture.text === text && capture.name === name,
    )
  ) {
    throw new Error(
      `Missing @${name} locals capture for ${JSON.stringify(text)} on row ${row + 1}`,
    );
  }
}

requireLocalCapture(0, "maximum", "local.definition.function");
requireLocalCapture(0, "left", "local.definition.parameter");
requireLocalCapture(1, "left", "local.reference");
requireLocalCapture(9, "index", "local.definition.var");
requireLocalCapture(30, "Counter", "local.definition.type");
requireLocalCapture(31, "value", "local.definition.field");
requireLocalCapture(38, "file_value", "local.definition.var");
requireLocalCapture(42, "count", "local.definition.field");
requireLocalCapture(48, "N", "local.definition.parameter");
requireLocalCapture(49, "N", "local.definition.parameter");
requireLocalCapture(51, "Renderable", "local.definition.type");
requireLocalCapture(61, "Sprite", "local.definition.type");
requireLocalCapture(84, "value", "local.definition.var");
requireLocalCapture(85, "value", "local.reference");
requireLocalCapture(92, "other", "local.definition.parameter");
requireLocalCapture(93, "other", "local.definition.parameter");
requireLocalCapture(103, "first", "local.definition.var");
requireLocalCapture(103, "second", "local.definition.var");
requireLocalCapture(104, "first", "local.reference");
requireLocalCapture(107, "sortable", "local.definition.type");
requireLocalCapture(107, "T", "local.definition.type");
requireLocalCapture(107, "T", "local.reference");
requireLocalCapture(119, "condition", "local.definition.parameter");
requireLocalCapture(120, "condition", "local.reference");
requireLocalCapture(135, "native_close", "local.definition.function");
requireLocalCapture(135, "descriptor", "local.definition.parameter");
requireLocalCapture(142, "writable", "local.definition.parameter");
requireLocalCapture(142, "readable", "local.definition.parameter");
requireLocalCapture(144, "alias", "local.definition.var");
requireLocalCapture(144, "writable", "local.reference");
requireLocalCapture(145, "alias", "local.reference");

process.stdout.write("GTI Tree-sitter highlight and locals captures passed.\n");
