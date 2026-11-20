#pragma once
#include "gti/ast.h"
#include <map>
#include <span>
namespace lang {
// Installation binds the file name at import. No producer path, type ID or
// predefined list of consumer substitutions is part of the recipe.
struct CompiledTemplateModule {
  std::string declarations;
  std::map<std::size_t, std::string> bodies;
};
CompiledTemplateModule compileTemplateModule(std::span<const StmtPtr>,
                                             std::string_view source);
void restoreTemplateBodies(std::span<const StmtPtr>,
                           const CompiledTemplateModule &,
                           std::string_view path);
std::string encodeTemplateModule(const CompiledTemplateModule &);
CompiledTemplateModule decodeTemplateModule(std::string_view);
inline constexpr std::string_view compiledTemplateMagic =
    "gti.template-unit.1\n";
} // namespace lang
