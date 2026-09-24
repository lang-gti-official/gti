import type { ComponentType } from "react";

export type ReferencePageKind =
  | "index"
  | "language"
  | "library-header"
  | "type"
  | "function"
  | "member"
  | "concept"
  | "diagnostic"
  | "tooling"
  | "command";

export interface ReferencePageMetadata {
  title: string;
  description: string;
  kind: ReferencePageKind;
  api: string[];
  examples: string[];
  since?: string;
}

export type CompilerDocumentationKind =
  | "namespace"
  | "namespace-alias"
  | "concept"
  | "type-alias"
  | "class"
  | "struct"
  | "interface"
  | "union"
  | "enum"
  | "enumerator"
  | "constructor"
  | "destructor"
  | "function"
  | "method"
  | "operator"
  | "field"
  | "global-variable";

export interface CompilerDocumentationTraits {
  ownership: "value" | "borrowed" | "unique" | "shared";
  drop: "trivial" | "lexical";
  copyable: boolean;
  movable: boolean;
  copyAssignable: boolean;
  moveAssignable: boolean;
  containsBorrowedState: boolean;
  transferCapable: boolean;
  shareCapable: boolean;
}

export interface CompilerDocumentationDeclaration {
  key: string;
  groupKey: string;
  kind: CompilerDocumentationKind;
  name: string;
  qualifiedName: string;
  ownerQualifiedName: string | null;
  signature: string | null;
  documentationMarkdown: string | null;
  traits: CompilerDocumentationTraits | null;
  source: {
    unit: string;
    line: number;
    start: number;
    end: number;
  };
  mutableBinding: boolean;
  staticMember: boolean;
}

export type ReferenceExample =
  | {
      id: string;
      title: string;
      description: string;
      mode: "compile";
      executionProfile?: "single-threaded" | "concurrent";
      sourcePath: string;
      sourceText: string;
    }
  | {
      id: string;
      title: string;
      description: string;
      mode: "run";
      executionProfile?: "single-threaded" | "concurrent";
      sourcePath: string;
      sourceText: string;
      stdoutPath?: string;
      expectedStdout?: string;
    }
  | {
      id: string;
      title: string;
      description: string;
      mode: "diagnostic";
      executionProfile?: "single-threaded" | "concurrent";
      sourcePath: string;
      sourceText: string;
      diagnostics: string[];
    };

export interface ReferenceNavigationEntry {
  route: string;
  title: string;
  description: string;
  kind: ReferencePageKind;
}

export interface ReferencePageRecord {
  route: string;
  sourcePath: string;
  metadata: ReferencePageMetadata;
  apiDeclarations: CompilerDocumentationDeclaration[];
  examples: ReferenceExample[];
}

export interface GeneratedReferencePage extends ReferencePageRecord {
  Content: ComponentType;
}
