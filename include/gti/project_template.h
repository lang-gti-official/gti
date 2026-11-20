#pragma once
#include "gti/compiled_template_body.h"
namespace lang {
// Ordinary packages never inherit the sysroot's trust. Their recipe is paired
// with the erased lexical tokens so the normal parser can validate its shape.
std::string encodeProjectTemplate(const CompiledTemplateModule &,
                                  std::string_view source);
CompiledTemplateModule decodeProjectTemplate(std::string_view bytes);
std::vector<Token> projectTemplateTokens(const CompiledTemplateModule &,
                                         std::span<const Token> interfaceTokens,
                                         std::string_view path);
void verifyProjectTemplate(const CompiledTemplateModule &,
                           std::span<const StmtPtr> declarations,
                           const LifetimeNames &lifetimes);
std::string projectTemplateModuleName(std::string_view declarations);
} // namespace lang
