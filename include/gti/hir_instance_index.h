#pragma once

#include "gti/semantic_analyzer.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace lang {

// O(1) lookup index over the concrete instances HIR lowering has already
// discovered, replacing linear scans whose structural type comparisons made
// instance discovery quadratic. Keys compare with exactly the equality the
// previous scans used; the ordered instance vectors in HirProgram remain
// the identity-assigning authority, and per ADR 006 this index is a lookup
// structure only - it is never iterated to produce output.
//
// Implemented in src/compiler/hir.cpp (the first compiled slice of HIR
// lowering); LLVM-enabled builds hash with llvm::hash_combine.
class HirInstanceIndex {
public:
  HirInstanceIndex();
  ~HirInstanceIndex();
  HirInstanceIndex(HirInstanceIndex &&other) noexcept;
  HirInstanceIndex &operator=(HirInstanceIndex &&other) noexcept;
  HirInstanceIndex(const HirInstanceIndex &) = delete;
  HirInstanceIndex &operator=(const HirInstanceIndex &) = delete;

  [[nodiscard]] std::optional<std::size_t>
  findClass(ClassId declaration, const std::vector<SemanticType> &typeArguments,
            const std::vector<CompileTimeValue> &valueArguments) const;
  void recordClass(ClassId declaration, std::vector<SemanticType> typeArguments,
                   std::vector<CompileTimeValue> valueArguments,
                   std::size_t instance);

  [[nodiscard]] std::optional<std::size_t>
  findFunction(FunctionId declaration,
               const std::vector<SemanticType> &classTypeArguments,
               const std::vector<CompileTimeValue> &classValueArguments,
               const std::vector<SemanticType> &functionTypeArguments) const;
  void recordFunction(FunctionId declaration,
                      std::vector<SemanticType> classTypeArguments,
                      std::vector<CompileTimeValue> classValueArguments,
                      std::vector<SemanticType> functionTypeArguments,
                      std::size_t instance);

  [[nodiscard]] std::optional<std::size_t>
  findConstructor(ConstructorId declaration, std::size_t owner) const;
  void recordConstructor(ConstructorId declaration, std::size_t owner,
                         std::size_t instance);

  [[nodiscard]] std::optional<std::size_t>
  findDestructor(std::size_t owner) const;
  void recordDestructor(std::size_t owner, std::size_t instance);

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation;
};

} // namespace lang
