import assert from "node:assert/strict";
import {
  cpSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  compileContentGraph,
  ContentContractError,
} from "../src/content/contract";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const validProject = path.join(testDirectory, "fixtures/valid-project");

test("a page owns its dedicated examples and compiler bindings", () => {
  const pages = compileContentGraph({
    projectRoot: validProject,
    contentRoot: "content",
    apiIndexPath: "api-index.json",
  });
  assert.equal(pages.length, 2);
  const typePage = pages.find((page) => page.route === "std/vector/vector");
  assert.ok(typePage);
  assert.deepEqual(typePage.metadata.api, [
    "std/vector|class|std::vector",
    "std/vector|method|std::vector::size",
  ]);
  assert.equal(typePage.metadata.since, "0.80.0");
  assert.equal(typePage.apiDeclarations.length, 2);
  assert.equal(
    typePage.apiDeclarations[0].signature,
    "class std::vector<std::movable T>",
  );
  assert.equal(typePage.examples.length, 1);
  assert.equal(typePage.examples[0].id, "construction-and-iteration");
  assert.equal(
    typePage.examples[0].sourcePath,
    "content/std/vector/vector/examples/construction-and-iteration/main.gti",
  );
  assert.match(typePage.examples[0].sourceText, /std::vector<int32_t>/);
  assert.equal(
    typePage.examples[0].mode === "run"
      ? typePage.examples[0].expectedStdout
      : undefined,
    "2\n",
  );
  const memberPage = pages.find(
    (page) => page.route === "std/vector/vector/size",
  );
  assert.ok(memberPage);
  assert.deepEqual(memberPage.metadata.api, [
    "std/vector|method|std::vector::size",
  ]);
  assert.equal(memberPage.metadata.since, "gti-20");
  assert.equal(memberPage.apiDeclarations.length, 1);
  assert.equal(memberPage.apiDeclarations[0].name, "size");
});

test("a type member binding requires one canonical child page", () => {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-contract-"),
  );
  try {
    cpSync(validProject, temporary, { recursive: true });
    rmSync(
      path.join(temporary, "content/std/vector/vector/size"),
      { recursive: true },
    );
    assert.throws(
      () =>
        compileContentGraph({
          projectRoot: temporary,
          contentRoot: "content",
          apiIndexPath: "api-index.json",
        }),
      (error: unknown) =>
        error instanceof ContentContractError &&
        error.message.includes("requires a dedicated child member page"),
    );
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("a member page must bind one member declaration group", () => {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-contract-"),
  );
  try {
    cpSync(validProject, temporary, { recursive: true });
    writeFileSync(
      path.join(temporary, "content/std/vector/vector/size/page.mdx"),
      `---
title: std::vector::size
description: Invalid member binding.
kind: member
api:
  - std/vector|class|std::vector
examples: []
---
`,
    );
    assert.throws(
      () =>
        compileContentGraph({
          projectRoot: temporary,
          contentRoot: "content",
          apiIndexPath: "api-index.json",
        }),
      (error: unknown) =>
        error instanceof ContentContractError &&
        error.message.includes("may bind only constructors"),
    );
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("since accepts release versions and editions but rejects free-form labels", () => {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-contract-"),
  );
  try {
    cpSync(validProject, temporary, { recursive: true });
    const page = path.join(
      temporary,
      "content/std/vector/vector/size/page.mdx",
    );
    writeFileSync(
      page,
      readFileSync(page, "utf8").replace("since: gti-20", "since: someday"),
    );
    assert.throws(
      () =>
        compileContentGraph({
          projectRoot: temporary,
          contentRoot: "content",
          apiIndexPath: "api-index.json",
        }),
      (error: unknown) =>
        error instanceof ContentContractError &&
        error.message.includes("Expected a semantic version"),
    );
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("API-bound pages fail closed without a compiler index", () => {
  assert.throws(
    () =>
      compileContentGraph({
        projectRoot: validProject,
        contentRoot: "content",
      }),
    (error: unknown) =>
      error instanceof ContentContractError &&
      error.message.includes("an API index is required"),
  );
});

test("unlisted example directories are rejected", () => {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-contract-"),
  );
  try {
    cpSync(validProject, temporary, { recursive: true });
    const orphan = path.join(
      temporary,
      "content/std/vector/vector/examples/shared-shortcut",
    );
    mkdirSync(orphan, { recursive: true });
    writeFileSync(
      path.join(orphan, "example.json"),
      JSON.stringify({
        title: "Orphan",
        description: "Must not be accepted.",
        mode: "compile",
      }),
    );
    writeFileSync(path.join(orphan, "main.gti"), "int main() { return 0; }\n");
    assert.throws(
      () =>
        compileContentGraph({
          projectRoot: temporary,
          contentRoot: "content",
          apiIndexPath: "api-index.json",
        }),
      (error: unknown) =>
        error instanceof ContentContractError &&
        error.message.includes("page-owned example is not listed"),
    );
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("example identifiers cannot escape their page", () => {
  const temporary = mkdtempSync(
    path.join(os.tmpdir(), "gti-reference-contract-"),
  );
  try {
    mkdirSync(path.join(temporary, "content/std/vector/vector"), {
      recursive: true,
    });
    writeFileSync(
      path.join(temporary, "content/std/vector/vector/page.mdx"),
      `---
title: std::vector
description: Invalid escaping example.
kind: type
api: []
examples:
  - ../shared
---
`,
    );
    assert.throws(
      () =>
        compileContentGraph({
          projectRoot: temporary,
          contentRoot: "content",
        }),
      (error: unknown) =>
        error instanceof ContentContractError &&
        error.message.includes("Invalid string"),
    );
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test(
  "page example roots cannot escape through a symlink",
  { skip: process.platform === "win32" },
  () => {
    const temporary = mkdtempSync(
      path.join(os.tmpdir(), "gti-reference-contract-"),
    );
    try {
      cpSync(validProject, temporary, { recursive: true });
      const examples = path.join(
        temporary,
        "content/std/vector/vector/examples",
      );
      rmSync(examples, { recursive: true });
      symlinkSync(
        path.join(
          validProject,
          "content/std/vector/vector/examples",
        ),
        examples,
        "dir",
      );
      assert.throws(
        () =>
          compileContentGraph({
            projectRoot: temporary,
            contentRoot: "content",
            apiIndexPath: "api-index.json",
          }),
        (error: unknown) =>
          error instanceof ContentContractError &&
          error.message.includes("expected a real, non-symlink directory"),
      );
    } finally {
      rmSync(temporary, { recursive: true, force: true });
    }
  },
);
