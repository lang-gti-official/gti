import { readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = dirname(dirname(fileURLToPath(import.meta.url)));

async function renderedPage(route: string): Promise<string> {
  return readFile(join(projectRoot, "out", route, "index.html"), "utf8");
}

function requireText(page: string, expected: string, route: string): void {
  if (!page.includes(expected)) {
    throw new Error(
      `${route} is missing rendered marker ${JSON.stringify(expected)}.`,
    );
  }
}

function rejectText(page: string, rejected: string, route: string): void {
  if (page.includes(rejected)) {
    throw new Error(`${route} still contains ${JSON.stringify(rejected)}.`);
  }
}

async function main(): Promise<void> {
  const home = await renderedPage("");
  requireText(home, "GTI reference", "/");
  requireText(home, "Reference contents", "/");
  for (const clutter of [
    "Published pages",
    "Compiler declarations",
    "Verified examples",
    "Explore std::vector",
    "Learn the language",
  ]) {
    rejectText(home, clutter, "/");
  }

  const standardLibrary = await renderedPage("std");
  requireText(standardLibrary, "<table", "/std/");
  requireText(standardLibrary, "<th>Include</th>", "/std/");
  rejectText(standardLibrary, "| Include | Current usable surface |", "/std/");

  const projectTooling = await renderedPage("tooling/projects");
  requireText(projectTooling, 'class="shiki', "/tooling/projects/");
  requireText(projectTooling, "--shiki-dark", "/tooling/projects/");

  const vectorExample = await renderedPage("std/vector/vector/push-back");
  requireText(vectorExample, 'class="shiki', "/std/vector/vector/push-back/");
  requireText(vectorExample, ">mut</span>", "/std/vector/vector/push-back/");

  console.log(
    "Validated rendered tables, reference index, and syntax highlighting.",
  );
}

void main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
