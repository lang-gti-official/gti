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

// Storage facts shared by initialization, atomic operations and host parking.
// This private scalar representation is not a C/C++ atomic-object ABI.
struct AtomicStorageLayout {
  SemanticType element;
  TypeLayout object;
  llvm::IntegerType *lane = nullptr;
};

struct MutexStorageLayout {
  SemanticType element;
  TypeLayout object;
  unsigned nativeIndex = 0;
  unsigned payloadIndex = 1;
  unsigned payloadActiveIndex = 2;
  static constexpr unsigned shellLive = 1;
  static constexpr unsigned payloadLive = 2;
};

struct PayloadLayout {
  TypeLayout type;
  llvm::StructType *storage = nullptr;
  std::vector<llvm::StructType *> variants;
  unsigned tagIndex = 0;
  unsigned payloadIndex = 1;
  // Verified variants have consecutive indices strictly below this encoding.
  // It denotes representation-only empty/moved state, never a source case.
  static constexpr std::uint32_t inactive = UINT32_MAX;
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
  [[nodiscard]] AtomicStorageLayout atomicStorage(const SemanticType &type);
  [[nodiscard]] MutexStorageLayout mutexStorage(const SemanticType &type);
  [[nodiscard]] const PayloadLayout &payload(const SemanticType &type);
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
  [[nodiscard]] unsigned captureIndex(const SemanticType &type,
                                      std::size_t capture) const;

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
  struct LambdaRecord {
    const MirLambdaInstance *instance = nullptr;
    llvm::StructType *storage = nullptr;
    bool building = false;
    std::optional<TypeLayout> layout;
  };
  Session &session;
  llvm::StructType *textView_ = nullptr;
  std::optional<SharedOwnerLayout> sharedOwner_;
  std::map<HirClassInstanceId, Record> records;
  std::unordered_map<ClassId, std::vector<HirClassInstanceId>> classes;
  struct PayloadRecord {
    bool building = false;
    std::optional<PayloadLayout> layout;
  };
  std::map<const MirPayloadEnum *, PayloadRecord> payloads;
  std::map<HirLambdaId, LambdaRecord> lambdas;
  [[nodiscard]] TypeLayout record(const SemanticType &type);
  [[nodiscard]] TypeLayout closure(const SemanticType &type);
  [[nodiscard]] TypeLayout describe(llvm::Type *storage,
                                    bool addressOnly = false,
                                    llvm::Type *value = nullptr) const;
};

} // namespace lang::native_llvm
