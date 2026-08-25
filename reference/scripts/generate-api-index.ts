import { existsSync, mkdirSync } from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

interface Arguments {
  docgen?: string;
  standardLibrary?: string;
  output?: string;
}

function parseArguments(argv: string[]): Arguments {
  const result: Arguments = {};
  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (argument === "--") {
      continue;
    }
    if (
      (argument === "--docgen" ||
        argument === "--stdlib" ||
        argument === "--output") &&
      index + 1 < argv.length
    ) {
      const value = argv[++index];
      if (argument === "--docgen") {
        result.docgen = value;
      } else if (argument === "--stdlib") {
        result.standardLibrary = value;
      } else {
        result.output = value;
      }
      continue;
    }
    throw new Error(`Unknown or incomplete argument '${argument}'.`);
  }
  return result;
}

function defaultDocgen(repositoryRoot: string): string | undefined {
  const executable =
    process.platform === "win32" ? "gti_docgen.exe" : "gti_docgen";
  const candidates = [
    process.env.GTI_DOCGEN,
    path.join(repositoryRoot, "build", executable),
    path.join(repositoryRoot, "build-reference", executable),
  ].filter((candidate): candidate is string => candidate !== undefined);
  return candidates.find((candidate) => existsSync(candidate));
}

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDirectory, "..");
const repositoryRoot = path.resolve(projectRoot, "..");

try {
  const args = parseArguments(process.argv.slice(2));
  const docgen =
    args.docgen === undefined
      ? defaultDocgen(repositoryRoot)
      : path.resolve(projectRoot, args.docgen);
  if (docgen === undefined || !existsSync(docgen)) {
    throw new Error(
      "gti_docgen was not found. Build it with " +
        "'cmake --build <build-dir> --target gti_docgen' or set GTI_DOCGEN.",
    );
  }
  const standardLibrary = path.resolve(
    projectRoot,
    args.standardLibrary ?? path.join(repositoryRoot, "stdlib"),
  );
  const output = path.resolve(
    projectRoot,
    args.output ?? ".cache/documentation-index.json",
  );
  mkdirSync(path.dirname(output), { recursive: true });
  const generated = spawnSync(
    docgen,
    ["--stdlib", standardLibrary, "--output", output],
    { stdio: "inherit" },
  );
  if (generated.error) {
    throw generated.error;
  }
  if (generated.status !== 0) {
    throw new Error(`gti_docgen exited with status ${generated.status}.`);
  }
  console.log(`Generated ${path.relative(projectRoot, output)}.`);
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
