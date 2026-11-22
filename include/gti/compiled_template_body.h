#pragma once
#include "gti/ast.h"
#include "gti/lifetime_names.h"
#include <map>
#include <span>
namespace lang {
struct EmbeddedFile;
// Snapshot data, never permission for a consumer to reopen a producer path.
struct CompiledEmbeddedFile {
  Token keyword;
  std::size_t storageOffset = 0;
  std::string spelling;
  std::shared_ptr<const std::string> bytes;
};
inline constexpr std::size_t compiledTemplateSyntaxLimit = 1024U * 1024U;
inline constexpr std::size_t compiledEmbeddedFileLimit = 16U * 1024U * 1024U;
inline constexpr std::size_t compiledEmbeddedTotalLimit = 64U * 1024U * 1024U;
inline constexpr std::size_t compiledTemplateArchiveLimit =
    2U * compiledTemplateSyntaxLimit + compiledEmbeddedTotalLimit;

// Installation binds the file name at import. No producer path, type ID or
// predefined list of consumer substitutions is part of the recipe.
struct CompiledTemplateModule {
  std::string declarations;
  std::map<std::size_t, std::string> bodies;
  std::optional<std::string> projectSyntax;
  LifetimeNames lifetimeNames;
  // Authenticated by the installed bundle receipt. Ordinary project recipes
  // use the normal parser to attest their syntax instead.
  std::string installedSourceDigest;
  std::vector<CompiledEmbeddedFile> embeddedFiles;
};
CompiledTemplateModule
compileTemplateModule(std::span<const StmtPtr>, std::string_view source,
                      const LifetimeNames &names = {},
                      std::span<const EmbeddedFile> embedded = {});
LifetimeNames compiledTemplateLifetimeNames(const CompiledTemplateModule &,
                                            std::string_view path);
// Return the source spelling needed by expression-text queries when a restored
// body contains a macro invocation.
std::optional<std::string> restoreTemplateBodies(std::span<const StmtPtr>,
                                                 const CompiledTemplateModule &,
                                                 std::string_view path);
// Attest every archived site against ordinary lexical syntax. Payload bytes
// were snapshotted at production; this never resolves or reads those paths.
void verifyCompiledEmbeddedFiles(const CompiledTemplateModule &,
                                 std::string_view source);
std::string encodeTemplateModule(const CompiledTemplateModule &);
CompiledTemplateModule decodeTemplateModule(std::string_view);
inline constexpr std::string_view compiledTemplateMagic =
    "gti.template-unit.3\n";
} // namespace lang
