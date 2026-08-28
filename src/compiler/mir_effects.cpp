#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {
namespace {

constexpr MirEffectTraits harmless() {
  return {
      .speculatable = true, .removableWhenUnused = true, .reorderable = true};
}

constexpr MirEffectTraits trapping() { return {.mayTrap = true}; }

constexpr MirEffectTraits unknownEffects() {
  return {.readsPlace = true,
          .writesPlace = true,
          .readsUnknownMemory = true,
          .writesUnknownMemory = true,
          .allocates = true,
          .invokesRuntime = true,
          .mayTrap = true,
          .startsLoan = true,
          .endsLoan = true,
          .dependsOnLoan = true,
          .copiesValue = true,
          .movesValue = true,
          .initializesValue = true,
          .dropsValue = true,
          .invokesUserCode = true,
          .targetDependent = true,
          .maySynchronize = true};
}

constexpr auto instructionEffects = std::to_array<MirEffectTraits>({
    harmless(),
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .copiesValue = true},
    MirEffectTraits{.writesPlace = true, .initializesValue = true},
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .mayTrap = true,
                    .copiesValue = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{
        .readsPlace = true, .writesPlace = true, .movesValue = true},
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    MirEffectTraits{.startsLoan = true, .dependsOnLoan = true},
    MirEffectTraits{},
    unknownEffects(),
    MirEffectTraits{
        .mayTrap = true, .initializesValue = true, .invokesUserCode = true},
    MirEffectTraits{.writesPlace = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true},
    MirEffectTraits{.endsLoan = true, .dependsOnLoan = true},
    MirEffectTraits{
        .movesValue = true, .initializesValue = true, .dropsValue = true},
    unknownEffects(), // CallBody invokes the exact target MIR body.
});

constexpr auto operationEffects = std::to_array<MirEffectTraits>({
    harmless(), // None is neutral when an instruction has no scalar operation.
    harmless(), // Literal
    harmless(), // EnumConstant
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    trapping(), // Index
    harmless(), // Identity
    MirEffectTraits{.mayTrap = true, .targetDependent = true},
    harmless(), // ExpectedHasValue
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    harmless(), // NativeCallback
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    MirEffectTraits{.copiesValue = true}, // PayloadExtract
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    MirEffectTraits{.targetDependent = true}, // AddressOf
    MirEffectTraits{.targetDependent = true}, // PointerAdd
    MirEffectTraits{.targetDependent = true}, // PointerSubtract
    MirEffectTraits{.targetDependent = true}, // PointerDifference
    harmless(),                               // Comma
    trapping(),                               // Add: checked integer overflow.
    trapping(),                               // Subtract
    trapping(),                               // Multiply
    trapping(),                               // Divide
    trapping(),                               // Remainder
    harmless(),                               // BitwiseAnd
    harmless(),                               // BitwiseOr
    harmless(),                               // BitwiseXor
    trapping(),                               // ShiftLeft
    trapping(),                               // ShiftRight
    harmless(),                               // Equal
    harmless(),                               // NotEqual
    harmless(),                               // Less
    harmless(),                               // LessEqual
    harmless(),                               // Greater
    harmless(),                               // GreaterEqual
    harmless(),                               // Positive
    trapping(),                               // Negate
    harmless(),                               // LogicalNot
    harmless(),                               // BitwiseNot
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .mayTrap = true,
                    .copiesValue = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
    MirEffectTraits{.readsPlace = true, .writesPlace = true, .mayTrap = true},
});

constexpr auto intrinsicEffects = std::to_array<MirEffectTraits>({
    harmless(), // None is neutral outside an ordinary call.
    MirEffectTraits{.mayTrap = true, .targetDependent = true},
    MirEffectTraits{.mayTrap = true, .targetDependent = true},
    MirEffectTraits{
        .mayTrap = true, .initializesValue = true, .invokesUserCode = true},
    MirEffectTraits{
        .readsPlace = true, .writesPlace = true, .movesValue = true},
    MirEffectTraits{.writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true},
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    MirEffectTraits{.readsPlace = true, .removableWhenUnused = true},
    // UniqueOwnerUpcast
    MirEffectTraits{
        .readsPlace = true, .writesPlace = true, .movesValue = true},
    MirEffectTraits{.writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true},
    // StorageTryAllocate reports every request failure as a status. The
    // destination changes only after a complete allocation succeeds.
    MirEffectTraits{.writesPlace = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .initializesValue = true},
    // StorageCapacity observes allocation metadata without failure.
    MirEffectTraits{.readsUnknownMemory = true, .invokesRuntime = true},
    // StorageInitialized observes one bitmap entry without failure. The
    // public wrapper owns its separate logical bounds check.
    MirEffectTraits{.readsUnknownMemory = true, .invokesRuntime = true},
    MirEffectTraits{.writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true,
                    .invokesUserCode = true},
    MirEffectTraits{.readsUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .startsLoan = true},
    MirEffectTraits{.readsUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .startsLoan = true},
    MirEffectTraits{.writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true},
    // StorageRelocate
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .invokesUserCode = true},
    // StorageShiftRight
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .invokesUserCode = true},
    // StorageShiftLeft
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .invokesUserCode = true},
    harmless(), // IntegerWrappingAdd
    harmless(), // IntegerWrappingSubtract
    harmless(), // IntegerWrappingMultiply
    harmless(), // IntegerSaturatingAdd
    harmless(), // IntegerSaturatingSubtract
    harmless(), // IntegerSaturatingMultiply
    harmless(), // IntegerCheckedAdd
    harmless(), // IntegerCheckedSubtract
    harmless(), // IntegerCheckedMultiply
    // StorageBoundsCheck: a pure comparison over two staged scalars whose
    // only effect is the defined-failure trap on an out-of-bounds index.
    MirEffectTraits{.mayTrap = true},
    // AllocatePrefixStorage
    MirEffectTraits{.writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true},
    // PrefixStorageAppend
    MirEffectTraits{.writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true,
                    .invokesUserCode = true},
    // PrefixStoragePop
    MirEffectTraits{.writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true},
    // PrefixStorageRead
    MirEffectTraits{.readsUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .startsLoan = true},
    // PrefixStorageReadMut
    MirEffectTraits{.readsUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .startsLoan = true},
    // PrefixStorageLength
    MirEffectTraits{.readsUnknownMemory = true, .invokesRuntime = true},
    // PrefixStorageRelocate
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .invokesUserCode = true},
    // PrefixStorageInsert
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .initializesValue = true,
                    .invokesUserCode = true},
    // PrefixStorageErase
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .dropsValue = true,
                    .invokesUserCode = true},
    // PrefixStorageTextView: a read-only owner-dependent view over the live
    // character prefix.
    MirEffectTraits{.readsUnknownMemory = true, .startsLoan = true},
    // StringViewSize: a pure read of the view's stored length.
    MirEffectTraits{},
    // StringViewEmpty
    MirEffectTraits{},
    // ExpectedValue: extraction terminates on the wrong state.
    MirEffectTraits{.mayTrap = true},
    // ExpectedError
    MirEffectTraits{.mayTrap = true},
    // ExpectedValueOr: observes state and copies the selected value.
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    // ArrayRangeBegin: a pure compiler-generated zero endpoint.
    MirEffectTraits{},
    // ArraySize: a pure read of the fixed extent.
    MirEffectTraits{},
    // ExpectedHasValue: a pure state observer.
    MirEffectTraits{},
    // ThreadHandleEmpty: creates an empty private capability value.
    MirEffectTraits{.initializesValue = true},
    // ThreadSpawn: synchronization metadata supplies the full barrier facts.
    MirEffectTraits{.movesValue = true, .initializesValue = true},
    // ThreadJoinable: reads the opaque handle state without synchronizing.
    MirEffectTraits{.readsUnknownMemory = true},
    // ThreadJoin: synchronization metadata supplies the join barrier.
    MirEffectTraits{},
    // ThreadAutoJoin: synchronization metadata supplies the join barrier.
    MirEffectTraits{},
    // AtomicLoad: synchronization metadata supplies the read barrier.
    MirEffectTraits{.initializesValue = true},
    // AtomicStore: synchronization metadata supplies the write barrier.
    MirEffectTraits{},
    // AtomicExchange: synchronization metadata supplies the RMW barrier.
    MirEffectTraits{.initializesValue = true},
    // AtomicCompareExchange: synchronization metadata supplies both barriers.
    MirEffectTraits{.initializesValue = true},
    // MutexLock: synchronization metadata supplies the acquire barrier.
    MirEffectTraits{.initializesValue = true},
    // MutexUnlock: synchronization metadata supplies the release barrier.
    MirEffectTraits{},
    // MutexGuardActive: observes the private token state.
    MirEffectTraits{.readsUnknownMemory = true, .invokesRuntime = true},
    // MutexGuardRead: publishes a tracked read-only loan.
    MirEffectTraits{
        .readsUnknownMemory = true, .invokesRuntime = true, .startsLoan = true},
    // MutexGuardReadMut: publishes a tracked exclusive loan.
    MirEffectTraits{
        .readsUnknownMemory = true, .invokesRuntime = true, .startsLoan = true},
    // MutexLockFailure: enters the defined-failure runtime path.
    MirEffectTraits{.invokesRuntime = true, .mayTrap = true},
});

constexpr auto synchronizationEffects = std::to_array<MirEffectTraits>({
    harmless(), // None
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .invokesUserCode = true,
                    .maySynchronize = true}, // ThreadSpawn
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .maySynchronize = true}, // ThreadJoin
    MirEffectTraits{.readsUnknownMemory = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicLoad
    MirEffectTraits{.writesUnknownMemory = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicStore
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicReadModifyWrite
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicCompareExchange
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .maySynchronize = true}, // MutexLock
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .maySynchronize = true}, // MutexUnlock
});

constexpr auto instructionNames = std::to_array<std::string_view>({
    "compute",
    "load",
    "initialize",
    "assign",
    "modify",
    "move",
    "borrow",
    "activate-borrow",
    "call-input",
    "call",
    "construct",
    "drop",
    "end-borrow",
    "lifecycle",
    "call-body",
});

constexpr auto operationNames = std::to_array<std::string_view>({
    "none",
    "literal",
    "enum-constant",
    "aggregate",
    "index",
    "identity",
    "convert",
    "expected-value",
    "closure",
    "native-callback",
    "payload-construct",
    "payload-extract",
    "unexpected",
    "address-of",
    "pointer-add",
    "pointer-subtract",
    "pointer-difference",
    "comma",
    "add",
    "subtract",
    "multiply",
    "divide",
    "remainder",
    "bitwise-and",
    "bitwise-or",
    "bitwise-xor",
    "shift-left",
    "shift-right",
    "equal",
    "not-equal",
    "less",
    "less-equal",
    "greater",
    "greater-equal",
    "positive",
    "negate",
    "logical-not",
    "bitwise-not",
    "assign",
    "add-assign",
    "subtract-assign",
    "multiply-assign",
    "divide-assign",
    "remainder-assign",
    "bitwise-and-assign",
    "bitwise-or-assign",
    "bitwise-xor-assign",
    "shift-left-assign",
    "shift-right-assign",
    "pre-increment",
    "pre-decrement",
    "post-increment",
    "post-decrement",
});

constexpr auto intrinsicNames = std::to_array<std::string_view>({
    "none",
    "numeric-type-parameter-conversion",
    "numeric-alias-conversion",
    "default-type-parameter-construction",
    "move",
    "allocate-unique-owner",
    "unique-owner-borrow",
    "unique-owner-borrow-mut",
    "unique-owner-is-null",
    "unique-owner-upcast",
    "allocate-storage",
    "storage-try-allocate",
    "storage-capacity",
    "storage-initialized",
    "storage-construct",
    "storage-read",
    "storage-read-mut",
    "storage-destroy",
    "storage-relocate",
    "storage-shift-right",
    "storage-shift-left",
    "integer-wrapping-add",
    "integer-wrapping-subtract",
    "integer-wrapping-multiply",
    "integer-saturating-add",
    "integer-saturating-subtract",
    "integer-saturating-multiply",
    "integer-checked-add",
    "integer-checked-subtract",
    "integer-checked-multiply",
    "storage-bounds-check",
    "allocate-prefix-storage",
    "prefix-storage-append",
    "prefix-storage-pop",
    "prefix-storage-read",
    "prefix-storage-read-mut",
    "prefix-storage-length",
    "prefix-storage-relocate",
    "prefix-storage-insert",
    "prefix-storage-erase",
    "prefix-storage-text-view",
    "string-view-size",
    "string-view-empty",
    "expected-value",
    "expected-error",
    "expected-value-or",
    "array-range-begin",
    "array-size",
    "expected-has-value",
    "thread-handle-empty",
    "thread-spawn",
    "thread-joinable",
    "thread-join",
    "thread-auto-join",
    "atomic-load",
    "atomic-store",
    "atomic-exchange",
    "atomic-compare-exchange",
    "mutex-lock",
    "mutex-unlock",
    "mutex-guard-active",
    "mutex-guard-read",
    "mutex-guard-read-mut",
    "mutex-lock-failure",
});

constexpr auto synchronizationNames = std::to_array<std::string_view>({
    "none",
    "thread-spawn",
    "thread-join",
    "atomic-load",
    "atomic-store",
    "atomic-read-modify-write",
    "atomic-compare-exchange",
    "mutex-lock",
    "mutex-unlock",
});

constexpr auto atomicMemoryOrderNames = std::to_array<std::string_view>({
    "relaxed",
    "acquire",
    "release",
    "acquire-release",
    "sequentially-consistent",
});

static_assert(instructionEffects.size() == mirInstructionKindCount);
static_assert(operationEffects.size() == mirOperationCount);
static_assert(intrinsicEffects.size() == intrinsicKindCount);
static_assert(synchronizationEffects.size() ==
              synchronizationOperationKindCount);
static_assert(instructionNames.size() == mirInstructionKindCount);
static_assert(operationNames.size() == mirOperationCount);
static_assert(intrinsicNames.size() == intrinsicKindCount);
static_assert(synchronizationNames.size() == synchronizationOperationKindCount);
static_assert(atomicMemoryOrderNames.size() == atomicMemoryOrderCount);

template <typename Enum, std::size_t Size>
[[nodiscard]] constexpr std::size_t
index(Enum value, const std::array<MirEffectTraits, Size> &) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] MirEffectTraits merge(MirEffectTraits left,
                                    const MirEffectTraits &right) {
  left.readsPlace |= right.readsPlace;
  left.writesPlace |= right.writesPlace;
  left.readsUnknownMemory |= right.readsUnknownMemory;
  left.writesUnknownMemory |= right.writesUnknownMemory;
  left.allocates |= right.allocates;
  left.invokesRuntime |= right.invokesRuntime;
  left.mayTrap |= right.mayTrap;
  left.startsLoan |= right.startsLoan;
  left.endsLoan |= right.endsLoan;
  left.dependsOnLoan |= right.dependsOnLoan;
  left.copiesValue |= right.copiesValue;
  left.movesValue |= right.movesValue;
  left.initializesValue |= right.initializesValue;
  left.dropsValue |= right.dropsValue;
  left.invokesUserCode |= right.invokesUserCode;
  left.targetDependent |= right.targetDependent;
  left.maySynchronize |= right.maySynchronize;
  left.speculatable = left.speculatable && right.speculatable;
  left.removableWhenUnused =
      left.removableWhenUnused && right.removableWhenUnused;
  left.reorderable = left.reorderable && right.reorderable;
  return left;
}

template <typename Enum, std::size_t Size>
[[nodiscard]] std::string_view
enumName(Enum value, const std::array<std::string_view, Size> &names) {
  const std::size_t valueIndex = static_cast<std::size_t>(value);
  return valueIndex < names.size() ? names[valueIndex] : "invalid";
}

template <typename Enum, std::size_t Size>
[[nodiscard]] MirEffectTraits
enumEffects(Enum value, const std::array<MirEffectTraits, Size> &table) {
  const std::size_t valueIndex = index(value, table);
  MirEffectTraits result =
      valueIndex < table.size() ? table[valueIndex] : unknownEffects();
  if (result.invokesRuntime || result.invokesUserCode) {
    result.maySynchronize = true;
  }
  return result;
}

} // namespace

std::string_view name(MirInstructionKind kind) {
  return enumName(kind, instructionNames);
}

std::string_view name(MirOperation operation) {
  return enumName(operation, operationNames);
}

std::string_view name(IntrinsicKind intrinsic) {
  return enumName(intrinsic, intrinsicNames);
}

std::string_view name(SynchronizationOperationKind kind) {
  return enumName(kind, synchronizationNames);
}

std::string_view name(AtomicMemoryOrder order) {
  return enumName(order, atomicMemoryOrderNames);
}

MirEffectTraits effects(MirInstructionKind kind) {
  return enumEffects(kind, instructionEffects);
}

MirEffectTraits effects(MirOperation operation) {
  return enumEffects(operation, operationEffects);
}

MirEffectTraits effects(IntrinsicKind intrinsic) {
  return enumEffects(intrinsic, intrinsicEffects);
}

MirEffectTraits effects(SynchronizationOperationKind kind) {
  return enumEffects(kind, synchronizationEffects);
}

namespace {

[[nodiscard]] MirEffectTraits
finishInstructionEffects(const MirInstruction &instruction,
                         MirEffectTraits result) {
  if (instruction.tombstone) {
    return harmless();
  }
  if (instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic != IntrinsicKind::None) {
    result = effects(instruction.intrinsic);
  } else if (instruction.kind == MirInstructionKind::Call &&
             instruction.synchronization.kind !=
                 SynchronizationOperationKind::None) {
    result = effects(instruction.synchronization.kind);
  } else if (result == MirEffectTraits{}) {
    result = effects(instruction.kind);
  }
  result = merge(result, effects(instruction.operation));
  result = merge(result, effects(instruction.synchronization.kind));
  if (instruction.hostedStartupOperation != 0) {
    // Generated hosted operations may read native wrapper state even when
    // their ordinary MIR opcode/operation pair is deliberately neutral.
    result = merge(result, unknownEffects());
  }

  for (const MirOperand &operand : instruction.operands) {
    switch (operand.kind) {
    case MirOperandKind::Value:
    case MirOperandKind::Constant:
    case MirOperandKind::Address:
      break;
    case MirOperandKind::Copy:
      result.readsPlace = true;
      result.copiesValue = true;
      break;
    case MirOperandKind::Move:
      result.readsPlace = true;
      result.writesPlace = true;
      result.movesValue = true;
      break;
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      result.readsPlace = true;
      result.startsLoan = true;
      break;
    case MirOperandKind::Loan:
      result.dependsOnLoan = true;
      break;
    }
  }
  if (instruction.receiver &&
      instruction.receiver->kind == MirOperandKind::Loan) {
    result.dependsOnLoan = true;
  }
  if (instruction.kind == MirInstructionKind::Construct) {
    result.copiesValue |= instruction.constructorKind == ConstructorKind::Copy;
    result.movesValue |= instruction.constructorKind == ConstructorKind::Move;
  }
  if (instruction.kind == MirInstructionKind::CallInput &&
      (instruction.callInputKind == HirCallInputKind::CopyValue ||
       instruction.callInputKind == HirCallInputKind::MoveValue)) {
    result = merge(result, unknownEffects());
    result.copiesValue |=
        instruction.callInputKind == HirCallInputKind::CopyValue;
    result.movesValue |=
        instruction.callInputKind == HirCallInputKind::MoveValue;
    result.initializesValue = true;
    result.maySynchronize = true;
  }
  if (!instruction.lifecycle.empty()) {
    result = merge(result, effects(MirInstructionKind::Lifecycle));
    if (std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                    [](const MirLifecycleEvent &event) {
                      return event.kind == MirLifecycleEventKind::Replace;
                    })) {
      // Replacement destroys the prior active value before publishing its
      // replacement. Until cleanup summaries exist, retain the same
      // conservative user-code/runtime ordering barrier as an explicit drop.
      result = merge(result, effects(MirInstructionKind::Drop));
    }
  }
  if (instruction.rawMemoryAccess) {
    if (instruction.kind == MirInstructionKind::Load) {
      result.readsUnknownMemory = true;
    } else {
      result.writesUnknownMemory = true;
      result.readsUnknownMemory |=
          instruction.kind == MirInstructionKind::Modify ||
          instruction.operation != MirOperation::Assign;
    }
    result.targetDependent = true;
  }
  if (instruction.unsafeOperation == UnsafeOperationKind::PointerArithmetic ||
      instruction.unsafeOperation == UnsafeOperationKind::AddressOf) {
    result.targetDependent = true;
  }
  if (!instruction.definedFailure.empty()) {
    result.mayTrap = true;
  }
  const bool observable =
      result.writesPlace || result.readsUnknownMemory ||
      result.writesUnknownMemory || result.allocates || result.invokesRuntime ||
      result.mayTrap || result.startsLoan || result.endsLoan ||
      result.dependsOnLoan || result.movesValue || result.initializesValue ||
      result.dropsValue || result.invokesUserCode || result.targetDependent ||
      result.maySynchronize;
  if (observable) {
    result.speculatable = false;
    result.removableWhenUnused = false;
    result.reorderable = false;
  }
  return result;
}

[[nodiscard]] MirEffectTraits
exactInvocationShell(const MirInstruction &instruction) {
  MirEffectTraits result;
  switch (instruction.kind) {
  case MirInstructionKind::Call:
  case MirInstructionKind::CallBody:
    result.invokesUserCode = true;
    break;
  case MirInstructionKind::Construct:
    result.initializesValue = true;
    result.invokesUserCode = true;
    break;
  case MirInstructionKind::Drop:
    result.writesPlace = true;
    result.dropsValue = true;
    result.invokesUserCode = true;
    break;
  default:
    result = effects(instruction.kind);
    break;
  }
  return finishInstructionEffects(instruction, result);
}

[[nodiscard]] const MirEffectSummary *
summaryForAddress(const MirProgram &program, MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        program.findFunctionInstance(address.owner);
    return instance != nullptr &&
                   instance->definitionKind == MirDefinitionKind::Source
               ? &instance->effectSummary
               : nullptr;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        program.findConstructorInstance(address.owner);
    return instance != nullptr &&
                   instance->definitionKind == MirDefinitionKind::Source
               ? &instance->effectSummary
               : nullptr;
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *instance =
        program.findDestructorInstance(address.owner);
    return instance != nullptr &&
                   instance->definitionKind == MirDefinitionKind::Source
               ? &instance->effectSummary
               : nullptr;
  }
  case MirBodyKind::Lambda: {
    const MirLambdaInstance *instance = program.findLambda(address.owner);
    return instance == nullptr ? nullptr : &instance->effectSummary;
  }
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::HostedStartup:
    return nullptr;
  }
  return nullptr;
}

struct ExactTargets {
  bool boundary = false;
  std::vector<MirBodyAddress> addresses;
};

[[nodiscard]] ExactTargets exactTargets(const MirProgram &program,
                                        const MirBody &body,
                                        const MirInstruction &instruction) {
  ExactTargets result;
  if (instruction.kind == MirInstructionKind::Call &&
      (instruction.intrinsic != IntrinsicKind::None ||
       instruction.synchronization.kind !=
           SynchronizationOperationKind::None)) {
    return result;
  }
  if (instruction.kind == MirInstructionKind::Call) {
    if (instruction.dispatch != CallDispatch::Static ||
        instruction.nativeCallbackAdapter) {
      result.boundary = true;
      return result;
    }
    if (instruction.functionTarget) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Function,
           .owner = static_cast<std::size_t>(*instruction.functionTarget)});
    } else if (instruction.constructorTarget) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Constructor,
           .owner = static_cast<std::size_t>(*instruction.constructorTarget)});
    } else if (instruction.lambdaTarget) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Lambda,
           .owner = static_cast<std::size_t>(*instruction.lambdaTarget)});
    } else if (instruction.bodyTarget) {
      result.addresses.push_back(*instruction.bodyTarget);
    } else {
      result.boundary = true;
    }
  } else if (instruction.kind == MirInstructionKind::Construct) {
    if (instruction.constructorTarget) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Constructor,
           .owner = static_cast<std::size_t>(*instruction.constructorTarget)});
    } else {
      result.boundary = true;
    }
  } else if (instruction.kind == MirInstructionKind::CallBody) {
    if (instruction.bodyTarget) {
      result.addresses.push_back(*instruction.bodyTarget);
    } else {
      result.boundary = true;
    }
  } else if (instruction.kind == MirInstructionKind::Drop) {
    for (const MirLifecycleEvent &event : instruction.lifecycle) {
      if (event.kind != MirLifecycleEventKind::Drop || event.source == 0 ||
          event.source > body.dropObligations.size()) {
        continue;
      }
      const MirDropObligation &obligation =
          body.dropObligations[event.source - 1];
      if (obligation.dropType.destructor) {
        result.addresses.push_back({.kind = MirBodyKind::Destructor,
                                    .owner = static_cast<std::size_t>(
                                        *obligation.dropType.destructor)});
      }
    }
    if (result.addresses.empty()) {
      result.boundary = true;
    }
  }
  std::sort(
      result.addresses.begin(), result.addresses.end(),
      [](MirBodyAddress left, MirBodyAddress right) {
        return std::pair{static_cast<std::size_t>(left.kind), left.owner} <
               std::pair{static_cast<std::size_t>(right.kind), right.owner};
      });
  result.addresses.erase(
      std::unique(result.addresses.begin(), result.addresses.end()),
      result.addresses.end());
  for (const MirBodyAddress address : result.addresses) {
    if (summaryForAddress(program, address) == nullptr) {
      result.boundary = true;
    }
  }
  return result;
}

[[nodiscard]] bool isInvocation(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct ||
         instruction.kind == MirInstructionKind::Drop ||
         instruction.kind == MirInstructionKind::CallBody;
}

struct EffectNode {
  MirBodyAddress address;
  const MirBody *body = nullptr;
  std::size_t parameterCount = 0;
  bool hasReceiver = false;
  bool exactDefinition = false;
  MirEffectSummary local;
  std::vector<std::size_t> callees;
  bool unknownBoundary = false;
};

[[nodiscard]] std::optional<std::size_t>
findNode(const std::vector<EffectNode> &nodes, MirBodyAddress address) {
  const auto found =
      std::find_if(nodes.begin(), nodes.end(), [&](const EffectNode &node) {
        return node.address == address;
      });
  return found == nodes.end()
             ? std::nullopt
             : std::optional<std::size_t>{static_cast<std::size_t>(
                   std::distance(nodes.begin(), found))};
}

[[nodiscard]] MirEffectSummary conservativeSummary(std::size_t arguments,
                                                   bool hasReceiver) {
  MirEffectSummary result;
  result.traits = unknownEffects();
  result.receiverEscapes = hasReceiver;
  result.argumentsEscape.assign(arguments, true);
  return result;
}

void mergeSummary(MirEffectSummary &left, const MirEffectSummary &right) {
  left.traits = merge(left.traits, right.traits);
}

[[nodiscard]] bool effectTraitsCover(const MirEffectTraits &claimed,
                                     const MirEffectTraits &required) {
#define GTI_EFFECT_REQUIRE(field) (!required.field || claimed.field)
#define GTI_EFFECT_GUARANTEE(field) (!claimed.field || required.field)
  return GTI_EFFECT_REQUIRE(readsPlace) && GTI_EFFECT_REQUIRE(writesPlace) &&
         GTI_EFFECT_REQUIRE(readsUnknownMemory) &&
         GTI_EFFECT_REQUIRE(writesUnknownMemory) &&
         GTI_EFFECT_REQUIRE(allocates) && GTI_EFFECT_REQUIRE(invokesRuntime) &&
         GTI_EFFECT_REQUIRE(mayTrap) && GTI_EFFECT_REQUIRE(startsLoan) &&
         GTI_EFFECT_REQUIRE(endsLoan) && GTI_EFFECT_REQUIRE(dependsOnLoan) &&
         GTI_EFFECT_REQUIRE(copiesValue) && GTI_EFFECT_REQUIRE(movesValue) &&
         GTI_EFFECT_REQUIRE(initializesValue) &&
         GTI_EFFECT_REQUIRE(dropsValue) &&
         GTI_EFFECT_REQUIRE(invokesUserCode) &&
         GTI_EFFECT_REQUIRE(targetDependent) &&
         GTI_EFFECT_REQUIRE(maySynchronize) &&
         GTI_EFFECT_GUARANTEE(speculatable) &&
         GTI_EFFECT_GUARANTEE(removableWhenUnused) &&
         GTI_EFFECT_GUARANTEE(reorderable);
#undef GTI_EFFECT_REQUIRE
#undef GTI_EFFECT_GUARANTEE
}

[[nodiscard]] bool summaryCovers(const MirEffectSummary &claimed,
                                 const MirEffectSummary &required) {
  return claimed.argumentsEscape.size() == required.argumentsEscape.size() &&
         effectTraitsCover(claimed.traits, required.traits) &&
         (!required.receiverEscapes || claimed.receiverEscapes) &&
         std::equal(required.argumentsEscape.begin(),
                    required.argumentsEscape.end(),
                    claimed.argumentsEscape.begin(),
                    [](bool requiredEscape, bool claimedEscape) {
                      return !requiredEscape || claimedEscape;
                    });
}

} // namespace

MirEffectTraits effects(const MirInstruction &instruction) {
  return finishInstructionEffects(instruction, {});
}

MirEffectTraits effects(const MirProgram &program,
                        const MirInstruction &instruction) {
  if (!isInvocation(instruction)) {
    return effects(instruction);
  }
  const MirBody *body = nullptr;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *candidate = findMirBody(program, address);
    if (candidate == nullptr) {
      continue;
    }
    const bool contains =
        std::any_of(candidate->blocks.begin(), candidate->blocks.end(),
                    [&](const MirBlock &block) {
                      return std::any_of(block.instructions.begin(),
                                         block.instructions.end(),
                                         [&](const MirInstruction &value) {
                                           return &value == &instruction;
                                         });
                    });
    if (contains) {
      body = candidate;
      break;
    }
  }
  if (body == nullptr) {
    return effects(instruction);
  }
  const ExactTargets targets = exactTargets(program, *body, instruction);
  if (targets.boundary || targets.addresses.empty()) {
    return effects(instruction);
  }
  MirEffectTraits result = exactInvocationShell(instruction);
  for (const MirBodyAddress target : targets.addresses) {
    result = merge(result, summaryForAddress(program, target)->traits);
  }
  return result;
}

std::optional<MirEffectSummary>
exactMirInvocationSummary(const MirProgram &program, const MirBody &body,
                          const MirInstruction &instruction) {
  if (!isInvocation(instruction)) {
    return std::nullopt;
  }
  const ExactTargets targets = exactTargets(program, body, instruction);
  if (targets.boundary || targets.addresses.empty()) {
    return std::nullopt;
  }
  std::optional<MirEffectSummary> result;
  for (const MirBodyAddress target : targets.addresses) {
    const MirEffectSummary *summary = summaryForAddress(program, target);
    if (summary == nullptr) {
      return std::nullopt;
    }
    if (!result) {
      result = *summary;
      continue;
    }
    result->traits = merge(result->traits, summary->traits);
    result->receiverEscapes |= summary->receiverEscapes;
    if (result->argumentsEscape.size() != summary->argumentsEscape.size()) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < result->argumentsEscape.size();
         ++index) {
      result->argumentsEscape[index] =
          result->argumentsEscape[index] || summary->argumentsEscape[index];
    }
  }
  return result;
}

MirProgramEffectSummaries
deriveMirProgramEffectSummaries(const MirProgram &program) {
  std::vector<EffectNode> nodes;
  nodes.reserve(program.functionInstances().size() +
                program.constructorInstances().size() +
                program.destructorInstances().size() +
                program.lambdaInstances().size());
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    nodes.push_back(
        {.address = {.kind = MirBodyKind::Function, .owner = instance.id},
         .body = &instance.body,
         .parameterCount = instance.parameterTypes.size(),
         .hasReceiver = instance.owner.has_value() && !instance.staticMember,
         .exactDefinition =
             instance.definitionKind == MirDefinitionKind::Source});
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    nodes.push_back(
        {.address = {.kind = MirBodyKind::Constructor, .owner = instance.id},
         .body = &instance.body,
         .parameterCount = instance.parameterTypes.size(),
         .hasReceiver = true,
         .exactDefinition =
             instance.definitionKind == MirDefinitionKind::Source});
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    nodes.push_back(
        {.address = {.kind = MirBodyKind::Destructor, .owner = instance.id},
         .body = &instance.body,
         .hasReceiver = true,
         .exactDefinition =
             instance.definitionKind == MirDefinitionKind::Source});
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    nodes.push_back(
        {.address = {.kind = MirBodyKind::Lambda, .owner = instance.id},
         .body = &instance.body,
         .parameterCount = instance.parameterTypes.size(),
         .hasReceiver = !instance.captureTypes.empty(),
         .exactDefinition = true});
  }

  for (EffectNode &node : nodes) {
    node.local.traits = harmless();
    node.local.argumentsEscape.assign(node.parameterCount, false);
    if (!node.exactDefinition || node.body == nullptr) {
      node.local = conservativeSummary(node.parameterCount, node.hasReceiver);
      node.unknownBoundary = true;
      continue;
    }

    if (node.address.kind == MirBodyKind::Function) {
      const MirFunctionInstance *function =
          program.findFunctionInstance(node.address.owner);
      if (function != nullptr) {
        node.local.receiverEscapes =
            function->returnBorrowOrigin == BorrowOriginKind::Receiver;
        if (function->returnBorrowOrigin == BorrowOriginKind::Argument &&
            function->returnBorrowParameter != 0 &&
            function->returnBorrowParameter <=
                node.local.argumentsEscape.size()) {
          node.local.argumentsEscape[function->returnBorrowParameter - 1] =
              true;
        }
      }
    } else if (node.address.kind == MirBodyKind::Constructor) {
      const MirConstructorInstance *constructor =
          program.findConstructorInstance(node.address.owner);
      if (constructor != nullptr) {
        node.local.receiverEscapes =
            constructor->borrowOrigin == BorrowOriginKind::Receiver;
        if (constructor->borrowOrigin == BorrowOriginKind::Argument &&
            constructor->borrowParameter != 0 &&
            constructor->borrowParameter <= node.local.argumentsEscape.size()) {
          node.local.argumentsEscape[constructor->borrowParameter - 1] = true;
        }
      }
    }

    for (const MirLoan &loan : node.body->loans) {
      if (!loan.escapes || loan.source == 0 ||
          loan.source > node.body->places.size()) {
        continue;
      }
      const MirPlace &source = node.body->places[loan.source - 1];
      if (source.root == MirPlaceRootKind::This) {
        node.local.receiverEscapes = true;
      }
      const auto parameter =
          std::find_if(node.body->places.begin(), node.body->places.end(),
                       [&](const MirPlace &place) {
                         return place.id == loan.source && place.binding != 0;
                       });
      if (parameter == node.body->places.end()) {
        continue;
      }
      const auto markParameter = [&](const auto &instance) {
        const auto found =
            std::find(instance.parameterBindings.begin(),
                      instance.parameterBindings.end(), parameter->binding);
        if (found != instance.parameterBindings.end()) {
          node.local.argumentsEscape[static_cast<std::size_t>(
              std::distance(instance.parameterBindings.begin(), found))] = true;
        }
      };
      if (node.address.kind == MirBodyKind::Function) {
        if (const MirFunctionInstance *function =
                program.findFunctionInstance(node.address.owner)) {
          markParameter(*function);
        }
      } else if (node.address.kind == MirBodyKind::Constructor) {
        if (const MirConstructorInstance *constructor =
                program.findConstructorInstance(node.address.owner)) {
          markParameter(*constructor);
        }
      } else if (node.address.kind == MirBodyKind::Lambda) {
        if (const MirLambdaInstance *lambda =
                program.findLambda(node.address.owner)) {
          markParameter(*lambda);
        }
      }
    }

    for (const MirBlock &block : node.body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const ExactTargets targets =
            exactTargets(program, *node.body, instruction);
        if (isInvocation(instruction) && !targets.addresses.empty() &&
            !targets.boundary) {
          node.local.traits =
              merge(node.local.traits, exactInvocationShell(instruction));
          for (const MirBodyAddress target : targets.addresses) {
            const std::optional<std::size_t> targetNode =
                findNode(nodes, target);
            if (!targetNode) {
              node.unknownBoundary = true;
              continue;
            }
            node.callees.push_back(*targetNode);
          }
        } else {
          const MirEffectTraits instructionEffects = effects(instruction);
          node.local.traits = merge(node.local.traits, instructionEffects);
          if (isInvocation(instruction) &&
              (targets.boundary || targets.addresses.empty()) &&
              instruction.intrinsic == IntrinsicKind::None &&
              instruction.synchronization.kind ==
                  SynchronizationOperationKind::None) {
            node.unknownBoundary = true;
          }
          if (instructionEffects.invokesUserCode &&
              !isInvocation(instruction)) {
            node.unknownBoundary = true;
          }
        }
      }
      switch (block.terminator.kind) {
      case MirTerminatorKind::PropagateFailure:
      case MirTerminatorKind::ContainFailure:
      case MirTerminatorKind::TerminateCleanupFailure:
      case MirTerminatorKind::Exit:
        node.local.traits.mayTrap = true;
        break;
      case MirTerminatorKind::None:
      case MirTerminatorKind::Goto:
      case MirTerminatorKind::Branch:
      case MirTerminatorKind::Switch:
      case MirTerminatorKind::Invoke:
      case MirTerminatorKind::Return:
      case MirTerminatorKind::Unreachable:
        break;
      }
    }
    std::sort(node.callees.begin(), node.callees.end());
    node.callees.erase(std::unique(node.callees.begin(), node.callees.end()),
                       node.callees.end());
    if (node.unknownBoundary || node.local.traits.writesUnknownMemory) {
      node.local.receiverEscapes = node.hasReceiver;
      std::fill(node.local.argumentsEscape.begin(),
                node.local.argumentsEscape.end(), true);
    }
  }

  std::vector<bool> recursive(nodes.size(), false);
  for (std::size_t start = 0; start < nodes.size(); ++start) {
    std::vector<std::size_t> pending = nodes[start].callees;
    std::vector<bool> visited(nodes.size(), false);
    while (!pending.empty() && !recursive[start]) {
      const std::size_t current = pending.back();
      pending.pop_back();
      if (current == start) {
        recursive[start] = true;
        break;
      }
      if (current >= nodes.size() || visited[current]) {
        continue;
      }
      visited[current] = true;
      pending.insert(pending.end(), nodes[current].callees.begin(),
                     nodes[current].callees.end());
    }
  }

  std::vector<MirEffectSummary> summaries;
  summaries.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    summaries.push_back(recursive[index]
                            ? conservativeSummary(nodes[index].parameterCount,
                                                  nodes[index].hasReceiver)
                            : nodes[index].local);
  }
  for (std::size_t round = 0; round <= nodes.size(); ++round) {
    std::vector<MirEffectSummary> next = summaries;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (recursive[index]) {
        continue;
      }
      for (const std::size_t callee : nodes[index].callees) {
        mergeSummary(next[index], summaries[callee]);
        if (summaries[callee].receiverEscapes ||
            std::any_of(summaries[callee].argumentsEscape.begin(),
                        summaries[callee].argumentsEscape.end(),
                        [](bool value) { return value; })) {
          next[index].receiverEscapes = nodes[index].hasReceiver;
          std::fill(next[index].argumentsEscape.begin(),
                    next[index].argumentsEscape.end(), true);
        }
      }
    }
    if (next == summaries) {
      break;
    }
    summaries = std::move(next);
    if (round == nodes.size()) {
      for (std::size_t index = 0; index < nodes.size(); ++index) {
        summaries[index] = conservativeSummary(nodes[index].parameterCount,
                                               nodes[index].hasReceiver);
      }
    }
  }

  MirProgramEffectSummaries result;
  std::size_t next = 0;
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    result.functions.push_back(std::move(summaries[next++]));
  }
  for (std::size_t index = 0; index < program.constructorInstances().size();
       ++index) {
    result.constructors.push_back(std::move(summaries[next++]));
  }
  for (std::size_t index = 0; index < program.destructorInstances().size();
       ++index) {
    result.destructors.push_back(std::move(summaries[next++]));
  }
  for (std::size_t index = 0; index < program.lambdaInstances().size();
       ++index) {
    result.lambdas.push_back(std::move(summaries[next++]));
  }
  return result;
}

MirVerificationResult verifyMirEffectSummaries(const MirProgram &program) {
  MirVerificationResult result;
  const MirProgramEffectSummaries required =
      deriveMirProgramEffectSummaries(program);
  const auto reject = [&](MirBodyAddress address, std::string message) {
    result.errors.push_back({.bodyKind = address.kind,
                             .owner = address.owner,
                             .message = std::move(message)});
  };
  if (required.functions.size() != program.functionInstances().size() ||
      required.constructors.size() != program.constructorInstances().size() ||
      required.destructors.size() != program.destructorInstances().size() ||
      required.lambdas.size() != program.lambdaInstances().size()) {
    reject({}, "derived MIR effect summary cardinality is invalid");
    return result;
  }
  for (std::size_t index = 0; index < required.functions.size(); ++index) {
    if (!summaryCovers(program.functionInstances()[index].effectSummary,
                       required.functions[index])) {
      reject({.kind = MirBodyKind::Function, .owner = index + 1},
             "function effect summary under-approximates its MIR body");
    }
  }
  for (std::size_t index = 0; index < required.constructors.size(); ++index) {
    if (!summaryCovers(program.constructorInstances()[index].effectSummary,
                       required.constructors[index])) {
      reject({.kind = MirBodyKind::Constructor, .owner = index + 1},
             "constructor effect summary under-approximates its MIR body");
    }
  }
  for (std::size_t index = 0; index < required.destructors.size(); ++index) {
    if (!summaryCovers(program.destructorInstances()[index].effectSummary,
                       required.destructors[index])) {
      reject({.kind = MirBodyKind::Destructor, .owner = index + 1},
             "destructor effect summary under-approximates its MIR body");
    }
  }
  for (std::size_t index = 0; index < required.lambdas.size(); ++index) {
    if (!summaryCovers(program.lambdaInstances()[index].effectSummary,
                       required.lambdas[index])) {
      reject({.kind = MirBodyKind::Lambda, .owner = index + 1},
             "lambda effect summary under-approximates its MIR body");
    }
  }
  return result;
}

} // namespace lang
