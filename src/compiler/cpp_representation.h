#pragma once

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <optional>
#include <string>
#include <string_view>

namespace lang {

// Deterministic C++ representation authorities over the sealed lowered
// program, per ADR 016. These are backend policy: no C++ spelling is stored in
// LoweredProgram itself.

inline constexpr std::string_view cppEmittedStandardNamespace = "__gti_std";

[[nodiscard]] std::string cppSemanticTypeSpelling(const LoweredProgram &program,
                                                  CppStandard standard,
                                                  const SemanticType &type);

// The exact unqualified name `CppEmitter` writes for a function declaration:
// comparison/assignment operator source spellings, runtime-binding and
// C-linkage passthrough names, the reserved `__gti_entry` hosted entry name,
// virtual-method source names, and the `__gti_fn_<id>_<name>` spelling for
// ordinary GTI functions.
[[nodiscard]] std::string
cppFunctionSpelling(const LoweredFunctionDeclaration &function,
                    std::string_view sourceName);

// Whether one sealed GTI function instance has a fully concrete C++ signature.
// This rejects dependent and lexical-closure types; the latter remain the one
// physical ABI case that requires a deduced native template carrier.
[[nodiscard]] bool
cppConcreteFunctionInstanceIsNameable(const LoweredProgram &program,
                                      const LoweredFunctionInstance &instance,
                                      const MirFunctionInstance &mirInstance);

// A deterministic, GTI-owned unqualified symbol for one nameable concrete
// free-function instance. The digest is derived from semantic declaration and
// concrete-argument/signature identity, never discovery-order IDs or native
// overload resolution. Non-source, non-generic, member, dependent, or
// incoherent instances return no spelling.
[[nodiscard]] std::optional<std::string>
cppConcreteFreeFunctionSpelling(const LoweredProgram &program,
                                const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function,
                                const LoweredFunctionInstance &instance,
                                const MirFunctionInstance &mirInstance);

// The exact holder and value spellings `CppEmitter` writes for
// internal-linkage static storage: the `__gti_static_<symbol>_<name>` holder
// and its `::value` member for namespace-scope statics wrapped in an
// initialization guard.
[[nodiscard]] std::string cppStaticStorageBaseSpelling(SymbolId symbol,
                                                       std::string_view name);
[[nodiscard]] std::string cppStaticStorageValueSpelling(SymbolId symbol,
                                                        std::string_view name);

} // namespace lang
