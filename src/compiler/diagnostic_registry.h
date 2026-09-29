#pragma once

#include "gti/diagnostic_registry_generated.h"

#include <initializer_list>
#include <string_view>

namespace lang {

Diagnostic
makeDiagnostic(DiagnosticId id, SourceSpan primary,
               std::initializer_list<std::string_view> arguments = {});
Diagnostic
makeDiagnostic(DiagnosticId id, const Token &token,
               std::initializer_list<std::string_view> arguments = {});

} // namespace lang
