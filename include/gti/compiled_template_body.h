#pragma once
#include "gti/ast.h"
#include "gti/lifetime_names.h"
#include <map>
#include <span>
namespace lang {
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
};
CompiledTemplateModule compileTemplateModule(std::span<const StmtPtr>,
                                             std::string_view source,
                                             const LifetimeNames &names = {});
LifetimeNames compiledTemplateLifetimeNames(const CompiledTemplateModule &,
                                            std::string_view path);
// Return the source spelling needed by expression-text queries when a restored
// body contains a macro invocation.
std::optional<std::string> restoreTemplateBodies(std::span<const StmtPtr>,
                                                 const CompiledTemplateModule &,
                                                 std::string_view path);
std::string encodeTemplateModule(const CompiledTemplateModule &);
CompiledTemplateModule decodeTemplateModule(std::string_view);
inline constexpr std::string_view compiledTemplateMagic =
    "gti.template-unit.2\n";
} // namespace lang
