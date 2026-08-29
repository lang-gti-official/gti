const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const executable = path.join(
  root,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "tree-sitter.cmd" : "tree-sitter",
);

function parse(file) {
  const result = spawnSync(executable, ["parse", "--quiet", file], {
    cwd: root,
    encoding: "utf8",
  });
  return {
    status: result.status,
    output: `${result.stdout ?? ""}${result.stderr ?? ""}`,
    error: result.error,
  };
}

function requireAccepted(file) {
  const result = parse(file);
  if (
    result.error ||
    result.status !== 0 ||
    /\b(?:ERROR|MISSING)\b/.test(result.output)
  ) {
    process.stderr.write(result.output);
    throw result.error ?? new Error(`${path.basename(file)} was not accepted`);
  }
}

function requireRejected(file) {
  const result = parse(file);
  if (
    !result.error &&
    result.status === 0 &&
    !/\b(?:ERROR|MISSING)\b/.test(result.output)
  ) {
    throw new Error(`${path.basename(file)} was unexpectedly accepted`);
  }
}

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "gti-tree-source-"));
try {
  const valid = path.join(temporary, "valid.gti");
  fs.writeFileSync(
    valid,
    Buffer.from(
      "\ufeffint main() {\r\n" +
        "  // café 😀\r\n" +
        '  std::println("héllo 😀");\r\n' +
        "  return 0;\r\n" +
        "}\r\n",
      "utf8",
    ),
  );
  requireAccepted(valid);

  const misplacedBom = path.join(temporary, "misplaced-bom.gti");
  fs.writeFileSync(misplacedBom, "int \ufeffvalue = 0;\n", "utf8");
  requireRejected(misplacedBom);

  const unicodeIdentifier = path.join(temporary, "unicode-identifier.gti");
  fs.writeFileSync(unicodeIdentifier, "int café = 0;\n", "utf8");
  requireRejected(unicodeIdentifier);

  const loneCarriageReturn = path.join(temporary, "lone-cr.gti");
  fs.writeFileSync(loneCarriageReturn, "int main() {\rreturn 0;}\n", "utf8");
  requireRejected(loneCarriageReturn);

  const invalidUtf8 = path.join(temporary, "invalid-utf8.gti");
  fs.writeFileSync(invalidUtf8, Buffer.from([0x69, 0x6e, 0x74, 0x20, 0x80]));
  requireRejected(invalidUtf8);
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}

process.stdout.write("GTI Tree-sitter source-text contract passed.\n");
