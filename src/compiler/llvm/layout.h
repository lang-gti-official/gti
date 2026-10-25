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

// Private representation only. Allocation, atomics and typed cleanup share
// these indices; this is not an exported or persistent ABI.
struct SharedOwnerLayout {
  TypeLayout handle;
  TypeLayout block;
  unsigned handleBlock = 0;
  unsigned handleView = 1;
  unsigned blockStrong = 0;
  unsigned blockAllocation = 1;
};

// All layout queries, allocation sizes and ABI address-only choices share this
// session-owned map. Opaque LLVM declarations are allocated before their
// bodies.
class Layouts final {
public:
  [[nodiscard]] TypeLayout get(const SemanticType &type);
  [[nodiscard]] llvm::PointerType *pointer() const;
  [[nodiscard]] const SharedOwnerLayout &sharedOwner();
  [[nodiscard]] const MirClassInstance &
  instance(const SemanticType &type) const;
  [[nodiscard]] unsigned fieldIndex(const SemanticType &type,
                                    SymbolId field) const;
  [[nodiscard]] unsigned baseIndex(const SemanticType &type,
                                   HirClassInstanceId base) const;
  [[nodiscard]] std::optional<unsigned>
  activeIndex(const SemanticType &type) const;

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
  std::optional<SharedOwnerLayout> sharedOwner_;
  std::map<HirClassInstanceId, Record> records;
  std::unordered_map<ClassId, std::vector<HirClassInstanceId>> classes;
  [[nodiscard]] TypeLayout record(const SemanticType &type);
  [[nodiscard]] TypeLayout describe(llvm::Type *storage,
                                    bool addressOnly = false,
                                    llvm::Type *value = nullptr) const;
};

} // namespace lang::native_llvm
