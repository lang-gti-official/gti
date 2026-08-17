#include "gti/optimization/effects.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace lang {
namespace {

constexpr MirEffectTraits harmless() {
  return {
      .speculatable = true, .removableWhenUnused = true, .reorderable = true};
}

constexpr MirEffectTraits trapping() { return {.mayTrap = true}; }

constexpr MirEffectTraits unknownEffects() {
  return {.readsUnknownMemory = true,
          .writesUnknownMemory = true,
          .invokesRuntime = true,
          .mayTrap = true,
          .invokesUserCode = true,
          .targetDependent = true};
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
    unknownEffects(), // PackFold executes exact user-function instances.
    MirEffectTraits{.copiesValue = true}, // PackExpansion
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
    "pack-fold",
    "pack-expansion",
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

MirEffectTraits effects(const MirInstruction &instruction) {
  MirEffectTraits result;
  if (instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic != IntrinsicKind::None) {
    result = effects(instruction.intrinsic);
  } else if (instruction.kind == MirInstructionKind::Call &&
             instruction.synchronization.kind !=
                 SynchronizationOperationKind::None) {
    result = effects(instruction.synchronization.kind);
  } else {
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

} // namespace lang
