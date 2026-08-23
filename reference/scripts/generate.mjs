#!/usr/bin/env node

// Extracts and VERIFIES the facts the reference renders, so a page cannot go
// stale without the next build failing.
//
// Two kinds of output land in src/generated/ (gitignored, rebuilt per run):
//
//   - extracted indexes: stdlib units, the diagnostic-code census, and the
//     language-section list, read from the repository sources; and
//   - verified evidence: every runnable example under examples/run/ is
//     compiled AND executed by the actual GTI compiler, its output captured;
//     every reproducer under examples/diagnostics/GTI-XXXXX.gti must fail
//     with exactly that code. Any mismatch aborts the build.
//
// The verifying compiler is $GTI or <repo>/build/gti. There is no fallback:
// a reference built without a compiler could not honour its own guarantee.
//
// Nothing here parses GTI beyond recognizing comments; the compiler owns the
// grammar. The full entity graph arrives with `gti --emit-api-json`.

import { execFileSync } from "node:child_process";
import {
  accessSync,
  constants,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
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

function fail(message) {
  console.error(`\nreference generation FAILED: ${message}`);
  process.exit(1);
}

// --- The verifying compiler --------------------------------------------------

const gti = process.env.GTI ?? join(repositoryRoot, "build", "gti");
try {
  accessSync(gti, constants.X_OK);
} catch {
  fail(
    `no executable GTI compiler at ${gti}. Build it (cmake --build build ` +
      `--target gti gti_runtime) or set $GTI. The reference refuses to ` +
      `build unverified: every example on the site must compile today.`,
  );
}

const environment = {
  ...process.env,
  GTI_STDLIB_PATH: join(repositoryRoot, "stdlib"),
  GTI_RUNTIME_INCLUDE: join(repositoryRoot, "runtime", "include"),
  GTI_RUNTIME_LIBRARY: join(repositoryRoot, "build", "libgti_runtime.a"),
  GTI_VENDOR_INCLUDE: join(
    repositoryRoot,
    "vendor",
    "expected_lite",
    "include",
  ),
};

function runCompiler(args, options = {}) {
  try {
    const stdout = execFileSync(gti, args, {
      encoding: "utf8",
      timeout: 180_000,
      env: environment,
      ...options,
    });
    return { status: 0, stdout, stderr: "" };
  } catch (error) {
    return {
      status: error.status ?? -1,
      stdout: error.stdout?.toString() ?? "",
      stderr: error.stderr?.toString() ?? "",
    };
  }
}

const gtiVersion = runCompiler(["--version"]).stdout.trim();
if (!gtiVersion.startsWith("gti ")) {
  fail(`${gti} did not report a version`);
}

let commit = "unknown";
try {
  commit = execFileSync("git", ["rev-parse", "--short", "HEAD"], {
    cwd: repositoryRoot,
    encoding: "utf8",
  }).trim();
} catch {}

write("version.json", { gtiVersion, commit });

// --- Verified runnable examples ----------------------------------------------

const runDirectory = join(here, "..", "examples", "run");
const scratch = mkdtempSync(join(tmpdir(), "gti-reference-"));
const examples = [];

for (const entry of readdirSync(runDirectory).sort()) {
  if (!entry.endsWith(".gti")) continue;
  const sourcePath = join(runDirectory, entry);
  const source = readFileSync(sourcePath, "utf8");

  const title = source.match(/^\/\/ title: (.+)$/m)?.[1] ?? entry;
  const unit = source.match(/^\/\/ unit: (.+)$/m)?.[1] ?? null;
  // The displayed source drops the metadata lines but keeps real comments.
  const displayed = source
    .split("\n")
    .filter((line) => !/^\/\/ (title|unit): /.test(line))
    .join("\n")
    .replace(/^\n+/, "");

  const binary = join(scratch, entry.replace(/\.gti$/, ""));
  const build = runCompiler([sourcePath, "-o", binary]);
  if (build.status !== 0) {
    fail(
      `example ${entry} does not compile with ${gtiVersion}:\n` +
        build.stderr.slice(0, 2000),
    );
  }
  let executed;
  try {
    executed = {
      status: 0,
      stdout: execFileSync(binary, [], { encoding: "utf8", timeout: 60_000 }),
    };
  } catch (error) {
    executed = {
      status: error.status ?? -1,
      stdout: error.stdout?.toString() ?? "",
    };
  }
  if (executed.status !== 0) {
    fail(`example ${entry} compiled but exited ${executed.status}`);
  }

  examples.push({
    name: entry.replace(/\.gti$/, ""),
    title,
    unit,
    source: displayed,
    output: executed.stdout,
  });
  console.log(`verified example ${entry}`);
}

write("examples.json", { verifiedWith: gtiVersion, examples });

// --- Verified diagnostic reproducers ------------------------------------------

const diagnosticsDirectory = join(here, "..", "examples", "diagnostics");
const reproducers = {};

for (const entry of readdirSync(diagnosticsDirectory).sort()) {
  const match = entry.match(/^(GTI-[A-Z][0-9]{4})\.gti$/);
  if (!match) continue;
  const code = match[1];
  const sourcePath = join(diagnosticsDirectory, entry);
  const source = readFileSync(sourcePath, "utf8");

  const attempt = runCompiler([
    "--emit-cpp",
    sourcePath,
    "-o",
    join(scratch, "rejected.cpp"),
  ]);
  if (attempt.status === 0) {
    fail(`reproducer ${entry} unexpectedly compiles; it must fail`);
  }
  const output = attempt.stderr + attempt.stdout;
  if (!output.includes(`error[${code}]`)) {
    const observed = output.match(/GTI-[A-Z][0-9]{4}/)?.[0] ?? "no code";
    fail(
      `reproducer ${entry} fails with ${observed}, not ${code}; ` +
        `the page would document the wrong rule`,
    );
  }
  // The first diagnostic line, with the absolute path trimmed to a basename,
  // shown on the page as the real compiler output.
  const firstLine = (attempt.stderr.split("\n")[0] ?? "").replace(
    /^\S*\//,
    "",
  );
  reproducers[code] = { source, firstLine };
  console.log(`verified reproducer ${entry} -> ${code}`);
}

rmSync(scratch, { recursive: true, force: true });

write("diagnostic-examples.json", {
  verifiedWith: gtiVersion,
  reproducers,
});

// --- Standard-library units ----------------------------------------------------

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
    const types = [
      ...text.matchAll(/^\s*(?:class|struct) ([a-z_][a-z0-9_]*)/gm),
    ]
      .map((match) => match[1])
      .filter((typeName, index, all) => all.indexOf(typeName) === index);

    // A verbatim synopsis: declaration-shaped lines from the unit's public
    // sections, quoted rather than reconstructed so nothing can be invented.
    const synopsis = [];
    let inPublic = false;
    let depth = 0;
    for (const raw of lines) {
      const line = raw.trimEnd();
      const trimmed = line.trim();
      if (/^(class|struct) /.test(trimmed) && trimmed.endsWith("{")) {
        synopsis.push(line.replace(/\s*\{$/, " {"));
        inPublic = trimmed.startsWith("struct");
        depth = 1;
        continue;
      }
      if (depth === 0) continue;
      if (trimmed === "public:") {
        inPublic = true;
        continue;
      }
      if (trimmed === "private:" || trimmed === "protected:") {
        inPublic = false;
        continue;
      }
      const opens = (trimmed.match(/\{/g) ?? []).length;
      const closes = (trimmed.match(/\}/g) ?? []).length;
      if (depth === 1 && trimmed.startsWith("};")) {
        synopsis.push("};");
        synopsis.push("");
        depth = 0;
        continue;
      }
      if (
        depth === 1 &&
        inPublic &&
        /^[a-zA-Z_~[]/.test(trimmed) &&
        (trimmed.includes("(") ||
          trimmed.startsWith("using ") ||
          trimmed.startsWith("static constexpr"))
      ) {
        synopsis.push(
          "  " +
            trimmed
              .replace(/\s*\{\}?$/, ";")
              .replace(/\s*\{ .*\}$/, ";")
              .replace(/;;$/, ";"),
        );
      }
      depth += opens - closes;
      if (depth < 0) depth = 0;
    }

    return {
      name,
      include: `<std/${name}>`,
      summary: summary.join(" ").trim(),
      types,
      synopsis: synopsis.join("\n").trim(),
      lineCount: lines.length,
    };
  });

write("units.json", { generatedFrom: "stdlib/std", units });

// --- Diagnostic codes ------------------------------------------------------------

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
        codes.set(code, {
          code,
          family: families[match[1]] ?? match[1],
          files: [],
        });
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

// --- Language specification sections ------------------------------------------

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
      markdown: text,
    };
  });

write("language.json", { generatedFrom: "docs/language", sections });
