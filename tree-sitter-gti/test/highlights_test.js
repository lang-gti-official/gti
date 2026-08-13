const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
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

function queryCaptures(name, input = fixture) {
  const query = path.resolve(root, "..", "queries", "gti", `${name}.scm`);
  const result = spawnSync(
    executable,
    ["query", query, input, "--captures"],
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
requireCapture(30, "no_transfer", "attribute");
requireCapture(30, "unsafe_share", "attribute");
requireCapture(185, "c_abi", "attribute");
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
requireCapture(157, "constexpr", "keyword.modifier");
requireCapture(157, "compile_extent", "variable");
requireCapture(183, "double", "type.builtin");
requireCapture(183, "0.1d", "number.float");
requireCapture(161, "constexpr", "keyword.modifier");
requireCapture(161, "count", "variable.member");
requireCapture(162, "constexpr", "keyword.modifier");
requireCapture(162, "deferred", "function.method");
requireCapture(165, "constexpr", "keyword.modifier");
requireCapture(165, "deferred_twice", "function");
requireCapture(168, "constexpr", "keyword.modifier");
requireCapture(175, "compatible", "type.definition");
requireCapture(175, "Left", "type.parameter");
requireCapture(175, "Right", "type.parameter");
requireCapture(175, "same_pair", "type");
requireCapture(178, "requires", "keyword");
requireCapture(178, "input_iterator", "type");
requireCapture(178, "Left", "type.parameter");
requireCapture(179, "&&", "operator");
requireCapture(179, "sentinel_for", "type");
requireCapture(179, "Right", "type.parameter");
requireCapture(179, "Left", "type.parameter");
requireCapture(181, "sizeof", "keyword.operator");
requireCapture(181, "int32_t", "type.builtin");
requireCapture(182, "alignof", "keyword.operator");
requireCapture(182, "const", "keyword.modifier");
requireCapture(182, "*", "operator");

const reservedFixture = path.join(__dirname, "reserved_identifiers.gti");
const reservedCaptures = queryCaptures("highlights", reservedFixture);
const tokenHeader = fs.readFileSync(
  path.resolve(root, "..", "include", "gti", "token.h"),
  "utf8",
);
const reservedBlock = tokenHeader.match(
  /cppReservedIdentifiers\[\]\{([\s\S]*?)\n\};/,
);
if (!reservedBlock) {
  throw new Error("Unable to read cppReservedIdentifiers from token.h");
}
const reservedSpellings = [
  ...reservedBlock[1].matchAll(/"([^"]+)"/g),
].map((match) => match[1]);
reservedSpellings.push("delete");
const reservedLines = fs
  .readFileSync(reservedFixture, "utf8")
  .trim()
  .split("\n");
if (reservedLines.length !== reservedSpellings.length) {
  throw new Error("Reserved-identifier fixture is out of sync with token.h");
}
for (const [row, spelling] of reservedSpellings.entries()) {
  if (reservedLines[row] !== `int ${spelling} = 1;`) {
    throw new Error(`Missing reserved fixture line for ${JSON.stringify(spelling)}`);
  }
  if (
    !reservedCaptures.some(
      (capture) =>
        capture.row === row &&
        capture.text === spelling &&
        capture.name === "keyword",
    )
  ) {
    throw new Error(
      `Missing @keyword capture for reserved identifier ${JSON.stringify(spelling)}`,
    );
  }
}

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
requireLocalCapture(157, "compile_extent", "local.definition.var");
requireLocalCapture(161, "count", "local.definition.field");
requireLocalCapture(162, "deferred", "local.definition.method");
requireLocalCapture(165, "deferred_twice", "local.definition.function");
requireLocalCapture(175, "compatible", "local.definition.type");
requireLocalCapture(175, "Left", "local.definition.type");
requireLocalCapture(175, "Right", "local.definition.type");
requireLocalCapture(177, "constrain", "local.definition.function");

const rainbowCaptures = queryCaptures("rainbow-delimiters");

function requireRainbowDelimiter(lineText, token, occurrence = 1) {
  const source = require("node:fs").readFileSync(fixture, "utf8");
  const lines = source.split("\n");
  const row = lines.indexOf(lineText);
  if (row < 0) {
    throw new Error(`Missing rainbow-delimiters fixture line: ${lineText}`);
  }

  let column = -1;
  let searchFrom = 0;
  for (let index = 0; index < occurrence; ++index) {
    column = lineText.indexOf(token, searchFrom);
    if (column < 0) {
      throw new Error(
        `Missing occurrence ${occurrence} of ${JSON.stringify(token)} on row ${row + 1}`,
      );
    }
    searchFrom = column + token.length;
  }

  if (
    !rainbowCaptures.some(
      (capture) =>
        capture.row === row &&
        capture.column === column &&
        capture.text === token &&
        capture.name === "delimiter",
    )
  ) {
    throw new Error(
      `Missing @delimiter capture for ${JSON.stringify(token)} on row ${row + 1}`,
    );
  }
}

requireRainbowDelimiter('@compiler_constraint("smoke")', "(");
requireRainbowDelimiter("concept compiler_only<T>;", "<");
requireRainbowDelimiter(
  "concept compatible<Left, Right> = std::same_pair<Left, Right>;",
  "<",
  2,
);
requireRainbowDelimiter(
  "concept sortable<T> = std::totally_ordered<T> && std::movable<T>;",
  "<",
  2,
);
requireRainbowDelimiter("  auto [first, second] = value;", "[");
requireRainbowDelimiter("  [[discard]] choose_lower<int>(1, 2);", "[[");
requireRainbowDelimiter(
  "[[no_transfer, unsafe_share]] class Counter {",
  "[[",
);
requireRainbowDelimiter("[[c_abi]] struct NativePoint {", "[[");
requireRainbowDelimiter(
  "constexpr uint64_t scalar_layout = sizeof(int32_t);",
  "(",
);

process.stdout.write(
  "GTI Tree-sitter highlight, locals, and rainbow captures passed.\n",
);
