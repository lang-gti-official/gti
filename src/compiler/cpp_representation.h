#pragma once

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <optional>
#include <string>
#include <string_view>

namespace lang {

[[nodiscard]] std::string
cppClassTemplateNameSpelling(const LoweredProgram &program, ClassId id);

// Deterministic C++ representation authorities over the sealed lowered
// program, per ADR 016. These are backend policy: no C++ spelling is stored in
// LoweredProgram itself.

inline constexpr std::string_view cppEmittedStandardNamespace = "__gti_std";

// A symbolic result keeps one status-ABI result channel even when a concrete
// instance returns void. Virtual declarations and overrides share that choice.
[[nodiscard]] bool
cppFunctionHasFailureResultChannel(const LoweredProgram &program,
                                   const LoweredFunctionDeclaration &function);

// A virtual slot whose declaration has a class-bound symbolic value result
// retains a defaulted internal output channel across every override. Native
// byte-transport results use that channel; other results keep ordinary returns.
[[nodiscard]] bool cppVirtualFamilyHasConditionalResultChannel(
    const LoweredProgram &program, const LoweredFunctionDeclaration &function);

[[nodiscard]] std::string cppSemanticTypeSpelling(const LoweredProgram &program,
                                                  CppStandard standard,
                                                  const SemanticType &type);

// Foreign declarations and native-layout fields retain their actual pointer
// ABI. Ordinary GTI values use inert integer-address carriers instead.
[[nodiscard]] std::string cppNativeTypeSpelling(const LoweredProgram &program,
                                                CppStandard standard,
                                                const SemanticType &type);

// Internal value transport must not evaluate a native object-pointer member
// after its pointee expires. This representation fact is transitive through
// inline aggregates, not through references or owning handles. It does not
// change semantic copyability, cleanup, failure effects, or the external ABI.
[[nodiscard]] bool cppRequiresBytePreservingTransport(const MirProgram &program,
                                                      const SemanticType &type);

// Native records cannot acquire custom C++ special members without changing
// their external ABI. Their direct internal values (including fixed arrays)
// therefore cross source function boundaries through reference/output slots.
// Ordinary containing carriers have representation-owned safe special members.
[[nodiscard]] bool cppRequiresIndirectValueTransport(const MirProgram &program,
                                                     const SemanticType &type);

// A retained native template's direct dependent field needs the same physical
// selection as a concrete native record or inline array thereof.
[[nodiscard]] bool cppUsesDependentByteField(const SemanticType &type);

// A deterministic GTI-owned class symbol for one concrete, nameable generic
// class application. The direct representation is deliberately limited to
// top-level source classes whose complete member type inventory can be emitted
// without a native template carrier. Nested type families and lexical-closure
// arguments return no spelling and retain their separately audited carrier.
[[nodiscard]] std::optional<std::string>
cppConcreteClassName(const LoweredProgram &program, const SemanticType &type);

// The fully qualified form of cppConcreteClassName in the emitted program
// namespace. This is the semantic type spelling used by fields, signatures,
// MIR places, and calls once GTI has selected a concrete class instance.
[[nodiscard]] std::optional<std::string>
cppConcreteClassTypeSpelling(const LoweredProgram &program,
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

// Artifact-local identity for one closure-bearing free-function instance.
// Native template parameters carry unnameable types only; each exact MIR body
// keeps its own symbol rather than sharing a source-template implementation.
[[nodiscard]] std::optional<std::string>
cppCallableFreeFunctionSpelling(const LoweredProgram &program,
                                const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function,
                                const LoweredFunctionInstance &instance,
                                const MirFunctionInstance &mirInstance);

// A deterministic unqualified symbol for one concrete member carried by a
// direct concrete generic class. Operators retain their language-required C++
// operator spelling; ordinary methods receive a digest that includes their
// concrete owner and exact method arguments/signature.
[[nodiscard]] std::optional<std::string>
cppConcreteMemberFunctionSpelling(const LoweredProgram &program,
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
