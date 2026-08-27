import type { LanguageRegistration } from "shiki";

/**
 * GTI deliberately keeps a C++-familiar lexical surface. This grammar layers
 * GTI's own keywords and directives over Shiki's maintained C++ grammar so
 * reference snippets do not silently present GTI-only syntax as plain text.
 */
export const gtiLanguage: LanguageRegistration = {
  name: "gti",
  displayName: "GTI",
  scopeName: "source.gti",
  fileTypes: ["gti"],
  repository: {},
  patterns: [
    {
      begin: "(/\\*)",
      beginCaptures: { "1": { name: "punctuation.definition.comment.gti" } },
      end: "(\\*/)",
      endCaptures: { "1": { name: "punctuation.definition.comment.gti" } },
      name: "comment.block.gti",
    },
    {
      match: "//.*$",
      name: "comment.line.double-slash.gti",
    },
    {
      begin: "\"",
      beginCaptures: { "0": { name: "punctuation.definition.string.begin.gti" } },
      end: "\"",
      endCaptures: { "0": { name: "punctuation.definition.string.end.gti" } },
      name: "string.quoted.double.gti",
      patterns: [{ match: "\\\\.", name: "constant.character.escape.gti" }],
    },
    {
      begin: "'",
      beginCaptures: { "0": { name: "punctuation.definition.string.begin.gti" } },
      end: "'",
      endCaptures: { "0": { name: "punctuation.definition.string.end.gti" } },
      name: "string.quoted.single.gti",
      patterns: [{ match: "\\\\.", name: "constant.character.escape.gti" }],
    },
    {
      captures: {
        "1": { name: "keyword.control.directive.gti" },
        "2": { name: "string.quoted.other.include.gti" },
      },
      match: "^\\s*(#include)\\s+(<[^>]+>|\"[^\"]+\")",
    },
    {
      match:
        "^\\s*#(?:define|undef|if|ifdef|ifndef|elif|else|endif|error)\\b",
      name: "keyword.control.directive.gti",
    },
    {
      match: "@(?:runtime|compiler_constraint)\\b|\\[\\[[^\\]]+\\]\\]",
      name: "storage.type.annotation.gti",
    },
    {
      match:
        "\\b(?:auto|bool|char|double|expected|float|int|int8_t|int16_t|int32_t|int64_t|int8|int16|int32|int64|nullptr_t|uint|uint8_t|uint16_t|uint32_t|uint64_t|uint8|uint16|uint32|uint64|void)\\b",
      name: "storage.type.builtin.gti",
    },
    {
      match:
        "\\b(?:class|concept|enum|interface|namespace|struct|union|using)\\b",
      name: "storage.type.gti",
    },
    {
      match:
        "\\b(?:const|constexpr|extern|mut|override|private|public|static|virtual)\\b",
      name: "storage.modifier.gti",
    },
    {
      match:
        "\\b(?:break|case|continue|default|do|else|for|if|requires|return|switch|unexpected|unsafe|while)\\b",
      name: "keyword.control.gti",
    },
    {
      match: "\\b(?:alignof|defined|operator|sizeof)\\b",
      name: "keyword.operator.word.gti",
    },
    {
      match: "\\b(?:false|nullptr|target|this|true)\\b",
      name: "constant.language.gti",
    },
    {
      match: "\\b(?:delete|default)\\b(?=\\s*;)",
      name: "storage.modifier.lifecycle.gti",
    },
    {
      match: "\\b(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|[0-9]+(?:\\.[0-9]+)?)\\b",
      name: "constant.numeric.gti",
    },
    {
      match: "\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()",
      name: "entity.name.function.gti",
    },
    {
      match: "\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*::)",
      name: "entity.name.namespace.gti",
    },
    {
      include: "source.cpp",
    },
  ],
};
