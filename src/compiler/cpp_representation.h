#pragma once

#include "gti/cpp_emitter.h"
#include "gti/semantic_analyzer.h"

#include <string>
#include <string_view>

namespace lang {

// Deterministic C++ representation authorities extracted from the
// compatibility emitter, per ADR 016. These are the single source of the
// spellings the transitional emitter writes today and the lowered-program
// representation tables consume tomorrow; the emitter delegates here, so an
// extracted spelling can never drift from an emitted one.

inline constexpr std::string_view cppEmittedStandardNamespace = "__gti_std";

// The exact spelling `CppEmitter` writes for a semantic type at the given
// C++ standard. Unknown and unrepresentable kinds spell as `void`, matching
// the emitter's historical conservative fallback.
[[nodiscard]] std::string
cppSemanticTypeSpelling(const SemanticModel &semantics, CppStandard standard,
                        const SemanticType &type);

// The exact unqualified name `CppEmitter` writes for a function declaration:
// comparison/assignment operator source spellings, runtime-binding and
// C-linkage passthrough names, the reserved `__gti_entry` hosted entry name,
// virtual-method source names, and the `__gti_fn_<id>_<name>` spelling for
// ordinary GTI functions.
[[nodiscard]] std::string cppFunctionSpelling(const SemanticModel &semantics,
                                              const FunctionDecl &function);

// The exact holder and value spellings `CppEmitter` writes for
// internal-linkage static storage: the `__gti_static_<symbol>_<name>` holder
// and its `::value` member for namespace-scope statics wrapped in an
// initialization guard.
[[nodiscard]] std::string cppStaticStorageBaseSpelling(SymbolId symbol,
                                                       std::string_view name);
[[nodiscard]] std::string cppStaticStorageValueSpelling(SymbolId symbol,
                                                        std::string_view name);

} // namespace lang
