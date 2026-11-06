#pragma once

#include "gti/semantic_model.h"
#include <span>
#include <string>
#include <string_view>

namespace lang {
// Complete contract for a whole-reference result tied to one formal argument.
// No transfer/drop is attached to the reference itself; the caller owns loans.
struct CompiledBorrowContract {
  BorrowOriginKind origin = BorrowOriginKind::None;
  std::size_t parameter = 0;
  AccessMode access = AccessMode::ReadOnly;
  friend bool operator==(const CompiledBorrowContract &,
                         const CompiledBorrowContract &) = default;
};
// Process-independent callable metadata. Function keys identify overloads;
// the result completes the callable ABI contract without changing identity.
struct CompiledScalarSignature {
  std::string key;
  SemanticType result = SemanticType::Int32;
  std::string typeMetadata;
  std::string resultRecipe;
  CompiledBorrowContract borrow;
  FunctionCodegenAttributes codegenAttributes;
  friend bool operator==(const CompiledScalarSignature &,
                         const CompiledScalarSignature &) = default;
};

[[nodiscard]] bool isCompiledScalarValueType(const SemanticType &);
[[nodiscard]] bool isCompiledScalarResultType(const SemanticType &);
// Check whether signature elision plus the bounded whole-argument result
// contract reconstructs all normalized source regions. This admits no new
// generic/nested lifetime interface profile and does not rewrite source MIR.
[[nodiscard]] bool isCompiledScalarLifetimeProfile(
    const SemanticType &result, std::span<const SemanticType> parameters,
    const LifetimeSignatureContract &, const CompiledBorrowContract &);
// Canonical GTI source spelling for the validated scalar interface profile.
[[nodiscard]] std::string_view compiledScalarTypeSpelling(const SemanticType &);
// Validate the structural borrow contract independently of source emission.
void validateCompiledBorrowContract(const SemanticType &result,
                                    std::span<const SemanticType> parameters,
                                    const CompiledBorrowContract &);
// Emit one validated declaration using ordinary source borrow annotations.
[[nodiscard]] std::string
compiledInterfaceCallableSource(const CompiledScalarSignature &);
} // namespace lang
