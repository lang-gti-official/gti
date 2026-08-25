import matter from "gray-matter";
import {
  existsSync,
  lstatSync,
  readFileSync,
  readdirSync,
} from "node:fs";
import path from "node:path";
import { z } from "zod";

import type {
  CompilerDocumentationDeclaration,
  ReferenceExample,
  ReferencePageMetadata,
  ReferencePageRecord,
} from "./types";

const routeSegment = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const diagnosticCode = /^GTI-[A-Z][0-9]{4}$/;

const ReferencePageMetadataSchema = z
  .object({
    title: z.string().trim().min(1),
    description: z.string().trim().min(1),
    kind: z.enum([
      "index",
      "language",
      "library-header",
      "type",
      "function",
      "member",
      "concept",
      "diagnostic",
    ]),
    api: z.array(z.string().trim().min(1)).default([]),
    examples: z.array(z.string().regex(routeSegment)).default([]),
    since: z.string().trim().min(1).optional(),
  })
  .strict()
  .superRefine((metadata, context) => {
    if (new Set(metadata.api).size !== metadata.api.length) {
      context.addIssue({
        code: "custom",
        path: ["api"],
        message: "API group keys must be unique.",
      });
    }
    if (new Set(metadata.examples).size !== metadata.examples.length) {
      context.addIssue({
        code: "custom",
        path: ["examples"],
        message: "Example identifiers must be unique.",
      });
    }
  });

const exampleCommon = {
  title: z.string().trim().min(1),
  description: z.string().trim().min(1),
};

const ExampleMetadataSchema = z.discriminatedUnion("mode", [
  z
    .object({
      ...exampleCommon,
      mode: z.literal("compile"),
    })
    .strict(),
  z
    .object({
      ...exampleCommon,
      mode: z.literal("run"),
      stdout: z.literal("stdout.txt").optional(),
    })
    .strict(),
  z
    .object({
      ...exampleCommon,
      mode: z.literal("diagnostic"),
      diagnostics: z
        .array(z.string().regex(diagnosticCode))
        .min(1)
        .refine((codes) => new Set(codes).size === codes.length, {
          message: "Expected diagnostic codes must be unique.",
        }),
    })
    .strict(),
]);

const DocumentationTraitsSchema = z
  .object({
    ownership: z.enum(["value", "borrowed", "unique", "shared"]),
    drop: z.enum(["trivial", "lexical"]),
    copyable: z.boolean(),
    movable: z.boolean(),
    copyAssignable: z.boolean(),
    moveAssignable: z.boolean(),
    containsBorrowedState: z.boolean(),
    transferCapable: z.boolean(),
    shareCapable: z.boolean(),
  })
  .strict();

const DocumentationDeclarationSchema = z
  .object({
    key: z.string().min(1),
    groupKey: z.string().min(1),
    kind: z.enum([
      "namespace",
      "namespace-alias",
      "concept",
      "type-alias",
      "class",
      "struct",
      "interface",
      "union",
      "enum",
      "enumerator",
      "constructor",
      "destructor",
      "function",
      "method",
      "operator",
      "field",
      "global-variable",
    ]),
    name: z.string().min(1),
    qualifiedName: z.string().min(1),
    ownerQualifiedName: z.string().min(1).nullable(),
    signature: z.string().min(1).nullable(),
    documentationMarkdown: z.string().min(1).nullable(),
    traits: DocumentationTraitsSchema.nullable(),
    source: z
      .object({
        unit: z.string().min(1),
        line: z.number().int().positive(),
        start: z.number().int().nonnegative(),
        end: z.number().int().nonnegative(),
      })
      .strict()
      .refine((source) => source.end >= source.start, {
        message: "Source end must not precede source start.",
      }),
    mutableBinding: z.boolean(),
    staticMember: z.boolean(),
  })
  .strict();

const DocumentationIndexSchema = z
  .object({
    schema: z.literal("gti.documentation-index"),
    version: z.literal(1),
    declarations: z.array(DocumentationDeclarationSchema),
  })
  .strict()
  .superRefine((index, context) => {
    const keys = new Set<string>();
    index.declarations.forEach((declaration, declarationIndex) => {
      if (!keys.add(declaration.key)) {
        context.addIssue({
          code: "custom",
          path: ["declarations", declarationIndex, "key"],
          message: `Duplicate documentation key '${declaration.key}'.`,
        });
      }
    });
  });

export interface CompileContentOptions {
  projectRoot: string;
  contentRoot: string;
  apiIndexPath?: string;
}

export class ContentContractError extends Error {
  constructor(readonly issues: string[]) {
    super(
      `Reference content is invalid:\n${issues
        .map((issue) => `- ${issue}`)
        .join("\n")}`,
    );
    this.name = "ContentContractError";
  }
}

function stablePath(root: string, target: string): string {
  return path.relative(root, target).split(path.sep).join("/");
}

function formatZodIssues(
  source: string,
  error: z.ZodError,
): string[] {
  return error.issues.map((issue) => {
    const location = issue.path.length > 0 ? `.${issue.path.join(".")}` : "";
    return `${source}${location}: ${issue.message}`;
  });
}

function readJson(file: string, issues: string[]): unknown {
  try {
    return JSON.parse(readFileSync(file, "utf8"));
  } catch (error) {
    issues.push(
      `${file}: ${error instanceof Error ? error.message : String(error)}`,
    );
    return undefined;
  }
}

function findPageFiles(directory: string, issues: string[]): string[] {
  if (!requireRegularDirectory(directory, issues)) {
    return [];
  }
  const result: string[] = [];
  const visit = (current: string) => {
    const entries = readdirSync(current, { withFileTypes: true }).sort((a, b) =>
      a.name.localeCompare(b.name),
    );
    for (const entry of entries) {
      const absolute = path.join(current, entry.name);
      if (entry.isSymbolicLink()) {
        issues.push(`${absolute}: symbolic links are not allowed in content.`);
        continue;
      }
      if (entry.isDirectory()) {
        visit(absolute);
      } else if (entry.isFile() && entry.name === "page.mdx") {
        result.push(absolute);
      }
    }
  };
  visit(directory);
  return result;
}

function requireRegularFile(file: string, issues: string[]): boolean {
  if (!existsSync(file)) {
    issues.push(`${file}: required file is missing.`);
    return false;
  }
  const status = lstatSync(file);
  if (status.isSymbolicLink() || !status.isFile()) {
    issues.push(`${file}: expected a regular, non-symlink file.`);
    return false;
  }
  return true;
}

function requireRegularDirectory(
  directory: string,
  issues: string[],
): boolean {
  if (!existsSync(directory)) {
    issues.push(`${directory}: required directory is missing.`);
    return false;
  }
  const status = lstatSync(directory);
  if (status.isSymbolicLink() || !status.isDirectory()) {
    issues.push(
      `${directory}: expected a real, non-symlink directory.`,
    );
    return false;
  }
  return true;
}

function loadApiGroups(
  apiIndexPath: string | undefined,
  projectRoot: string,
  issues: string[],
): Map<string, CompilerDocumentationDeclaration[]> | undefined {
  if (!apiIndexPath) {
    return undefined;
  }
  const absolute = path.resolve(projectRoot, apiIndexPath);
  if (!requireRegularFile(absolute, issues)) {
    return new Map();
  }
  const parsed = DocumentationIndexSchema.safeParse(readJson(absolute, issues));
  if (!parsed.success) {
    issues.push(...formatZodIssues(stablePath(projectRoot, absolute), parsed.error));
    return new Map();
  }
  const groups = new Map<string, CompilerDocumentationDeclaration[]>();
  for (const declaration of parsed.data.declarations) {
    const group = groups.get(declaration.groupKey) ?? [];
    group.push(declaration);
    groups.set(declaration.groupKey, group);
  }
  return groups;
}

function loadExample(
  projectRoot: string,
  pageDirectory: string,
  id: string,
  issues: string[],
): ReferenceExample | undefined {
  const examplesDirectory = path.join(pageDirectory, "examples");
  if (!requireRegularDirectory(examplesDirectory, issues)) {
    return undefined;
  }
  const directory = path.join(examplesDirectory, id);
  if (!requireRegularDirectory(directory, issues)) {
    return undefined;
  }
  const metadataPath = path.join(directory, "example.json");
  const sourcePath = path.join(directory, "main.gti");
  const metadataPresent = requireRegularFile(metadataPath, issues);
  const sourcePresent = requireRegularFile(sourcePath, issues);
  if (!metadataPresent || !sourcePresent) {
    return undefined;
  }
  const parsed = ExampleMetadataSchema.safeParse(readJson(metadataPath, issues));
  if (!parsed.success) {
    issues.push(
      ...formatZodIssues(stablePath(projectRoot, metadataPath), parsed.error),
    );
    return undefined;
  }
  const common = {
    id,
    title: parsed.data.title,
    description: parsed.data.description,
    sourcePath: stablePath(projectRoot, sourcePath),
    sourceText: readFileSync(sourcePath, "utf8"),
  };
  if (parsed.data.mode === "compile") {
    return { ...common, mode: "compile" };
  }
  if (parsed.data.mode === "diagnostic") {
    return {
      ...common,
      mode: "diagnostic",
      diagnostics: parsed.data.diagnostics,
    };
  }
  const stdoutPath =
    parsed.data.stdout === undefined
      ? undefined
      : path.join(directory, parsed.data.stdout);
  if (stdoutPath !== undefined && !requireRegularFile(stdoutPath, issues)) {
    return undefined;
  }
  return {
    ...common,
    mode: "run",
    ...(stdoutPath === undefined
      ? {}
      : {
          stdoutPath: stablePath(projectRoot, stdoutPath),
          expectedStdout: readFileSync(stdoutPath, "utf8"),
        }),
  };
}

function orphanExampleIds(pageDirectory: string): string[] {
  const examplesDirectory = path.join(pageDirectory, "examples");
  if (!existsSync(examplesDirectory)) {
    return [];
  }
  const status = lstatSync(examplesDirectory);
  if (status.isSymbolicLink() || !status.isDirectory()) {
    return [];
  }
  return readdirSync(examplesDirectory, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .sort();
}

export function compileContentGraph({
  projectRoot,
  contentRoot,
  apiIndexPath,
}: CompileContentOptions): ReferencePageRecord[] {
  const absoluteProjectRoot = path.resolve(projectRoot);
  const absoluteContentRoot = path.resolve(absoluteProjectRoot, contentRoot);
  const issues: string[] = [];
  const apiGroups = loadApiGroups(
    apiIndexPath,
    absoluteProjectRoot,
    issues,
  );
  const pages: ReferencePageRecord[] = [];

  for (const pagePath of findPageFiles(absoluteContentRoot, issues)) {
    const pageDirectory = path.dirname(pagePath);
    const relativeDirectory = path.relative(absoluteContentRoot, pageDirectory);
    const segments = relativeDirectory.split(path.sep);
    const sourceLabel = stablePath(absoluteProjectRoot, pagePath);
    if (
      relativeDirectory.length === 0 ||
      segments.some((segment) => !routeSegment.test(segment))
    ) {
      issues.push(
        `${sourceLabel}: page directories must be lower-case URL segments.`,
      );
      continue;
    }

    let parsedMatter: ReturnType<typeof matter>;
    try {
      parsedMatter = matter(readFileSync(pagePath, "utf8"));
    } catch (error) {
      issues.push(
        `${sourceLabel}: ${error instanceof Error ? error.message : String(error)}`,
      );
      continue;
    }
    const metadataResult = ReferencePageMetadataSchema.safeParse(
      parsedMatter.data,
    );
    if (!metadataResult.success) {
      issues.push(...formatZodIssues(sourceLabel, metadataResult.error));
      continue;
    }
    const metadata: ReferencePageMetadata = metadataResult.data;
    const apiDeclarations: CompilerDocumentationDeclaration[] = [];
    if (metadata.api.length > 0 && apiGroups === undefined) {
      issues.push(
        `${sourceLabel}.api: an API index is required to validate compiler bindings.`,
      );
    } else if (apiGroups !== undefined) {
      for (const binding of metadata.api) {
        const declarations = apiGroups.get(binding);
        if (declarations === undefined) {
          issues.push(
            `${sourceLabel}.api: compiler group '${binding}' does not exist.`,
          );
        } else {
          apiDeclarations.push(...declarations);
        }
      }
    }

    const declaredExamples = new Set(metadata.examples);
    for (const candidate of orphanExampleIds(pageDirectory)) {
      if (!declaredExamples.has(candidate)) {
        issues.push(
          `${stablePath(
            absoluteProjectRoot,
            path.join(pageDirectory, "examples", candidate),
          )}: page-owned example is not listed in page.mdx.`,
        );
      }
    }
    const examples = metadata.examples
      .map((id) => loadExample(absoluteProjectRoot, pageDirectory, id, issues))
      .filter((example): example is ReferenceExample => example !== undefined);
    pages.push({
      route: segments.join("/"),
      sourcePath: sourceLabel,
      metadata,
      apiDeclarations,
      examples,
    });
  }

  pages.sort((left, right) => left.route.localeCompare(right.route));
  const duplicateRoutes = pages.filter(
    (page, index) => index > 0 && pages[index - 1].route === page.route,
  );
  for (const duplicate of duplicateRoutes) {
    issues.push(`${duplicate.sourcePath}: duplicate route '${duplicate.route}'.`);
  }
  if (issues.length > 0) {
    throw new ContentContractError(issues.sort());
  }
  return pages;
}
