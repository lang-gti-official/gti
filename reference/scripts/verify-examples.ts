import {
  existsSync,
  mkdtempSync,
  rmSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

import {
  compileContentGraph,
  ContentContractError,
} from "../src/content/contract";
import type { ReferenceExample } from "../src/content/types";

interface Arguments {
  gti: string;
  apiIndexPath?: string;
  contentRoot: string;
}

function parseArguments(argv: string[]): Arguments {
  const result: Arguments = {
    gti: process.env.GTI ?? "gti",
    contentRoot: process.env.GTI_REFERENCE_CONTENT ?? "content",
  };
  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (argument === "--") {
      continue;
    }
    if (
      (argument === "--gti" ||
        argument === "--api" ||
        argument === "--content") &&
      index + 1 < argv.length
    ) {
      if (argument === "--gti") {
        result.gti = argv[++index];
      } else if (argument === "--content") {
        result.contentRoot = argv[++index];
      } else {
        result.apiIndexPath = argv[++index];
      }
      continue;
    }
    throw new Error(`Unknown or incomplete argument '${argument}'.`);
  }
  return result;
}

function processOutput(result: ReturnType<typeof spawnSync>): string {
  return `${result.stdout ?? ""}${result.stderr ?? ""}`;
}

function requireStatus(
  condition: boolean,
  label: string,
  output: string,
): void {
  if (!condition) {
    throw new Error(`${label}\n${output}`);
  }
}

function verifyExample(
  gti: string,
  projectRoot: string,
  repositoryRoot: string,
  example: ReferenceExample,
  index: number,
): void {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-example-"),
  );
  try {
    const executable = path.join(
      temporary,
      process.platform === "win32"
        ? `example-${index}.exe`
        : `example-${index}`,
    );
    const source = path.resolve(projectRoot, example.sourcePath);
    const compilation = spawnSync(gti, [source, "-o", executable], {
      cwd: repositoryRoot,
      encoding: "utf8",
      env: process.env,
    });
    if (compilation.error) {
      throw compilation.error;
    }
    const compilationOutput = processOutput(compilation);
    if (example.mode === "diagnostic") {
      requireStatus(
        compilation.status !== 0,
        `${example.sourcePath}: expected compilation to fail.`,
        compilationOutput,
      );
      for (const code of example.diagnostics) {
        requireStatus(
          compilationOutput.includes(code),
          `${example.sourcePath}: missing expected diagnostic ${code}.`,
          compilationOutput,
        );
      }
      return;
    }
    requireStatus(
      compilation.status === 0,
      `${example.sourcePath}: compilation failed.`,
      compilationOutput,
    );
    if (example.mode === "compile") {
      return;
    }
    const executed = spawnSync(executable, [], {
      cwd: temporary,
      encoding: "utf8",
      env: process.env,
    });
    if (executed.error) {
      throw executed.error;
    }
    const executionOutput = processOutput(executed);
    requireStatus(
      executed.status === 0,
      `${example.sourcePath}: example exited with status ${executed.status}.`,
      executionOutput,
    );
    if (example.stdoutPath !== undefined) {
      const expected = example.expectedStdout;
      if (expected === undefined) {
        throw new Error(
          `${example.stdoutPath}: expected output was not projected into the content graph.`,
        );
      }
      const mismatch =
        `${example.sourcePath}: stdout did not match ${example.stdoutPath}.\n` +
        `expected: ${JSON.stringify(expected)}\n` +
        `actual:   ${JSON.stringify(executed.stdout)}`;
      requireStatus(executed.stdout === expected, mismatch, executionOutput);
    }
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
}

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDirectory, "..");
const repositoryRoot = path.resolve(projectRoot, "..");

try {
  const args = parseArguments(process.argv.slice(2));
  const defaultApiIndex = path.join(
    projectRoot,
    ".cache/documentation-index.json",
  );
  const apiIndexPath =
    args.apiIndexPath ??
    process.env.GTI_DOCUMENTATION_INDEX ??
    (existsSync(defaultApiIndex) ? defaultApiIndex : undefined);
  const gti =
    path.isAbsolute(args.gti) || !/[\\/]/.test(args.gti)
      ? args.gti
      : path.resolve(projectRoot, args.gti);
  const pages = compileContentGraph({
    projectRoot,
    contentRoot: args.contentRoot,
    apiIndexPath,
  });
  const examples = pages.flatMap((page) => page.examples);
  examples.forEach((example, index) =>
    verifyExample(gti, projectRoot, repositoryRoot, example, index),
  );
  console.log(
    `Verified ${examples.length} page-owned example${examples.length === 1 ? "" : "s"}.`,
  );
} catch (error) {
  if (error instanceof ContentContractError) {
    console.error(error.message);
  } else {
    console.error(error instanceof Error ? error.message : String(error));
  }
  process.exitCode = 1;
}
