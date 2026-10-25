#pragma once

#include "session.h"

#include <llvm/IR/DerivedTypes.h>

#include <map>
#include <unordered_map>

namespace lang::native_llvm {

struct TypeLayout {
  llvm::Type *value = nullptr;
  llvm::Type *storage = nullptr;
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  std::uint64_t stride = 0;
  bool addressOnly = false;
  std::vector<std::uint64_t> fieldOffsets;
};

// All layout queries, allocation sizes and ABI address-only choices share this
// session-owned map. Opaque LLVM declarations are allocated before their
// bodies.
class Layouts final {
public:
  [[nodiscard]] TypeLayout get(const SemanticType &type);
  [[nodiscard]] llvm::PointerType *pointer() const;

private:
  friend class Session;
  explicit Layouts(Session &session);
  Layouts(const Layouts &) = delete;
  Layouts &operator=(const Layouts &) = delete;
  struct Record {
    const MirClassInstance *instance = nullptr;
    llvm::StructType *storage = nullptr;
    bool building = false;
    std::optional<TypeLayout> layout;
  };
  Session &session;
  llvm::StructType *textView_ = nullptr;
  std::map<HirClassInstanceId, Record> records;
  std::unordered_map<ClassId, std::vector<HirClassInstanceId>> classes;
  [[nodiscard]] TypeLayout record(const SemanticType &type);
  [[nodiscard]] TypeLayout describe(llvm::Type *storage,
                                    bool addressOnly = false,
                                    llvm::Type *value = nullptr) const;
};

} // namespace lang::native_llvm
