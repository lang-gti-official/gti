#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "mir_verification_queries.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_set>
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
    MirEffectTraits{.writesPlace = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true}, // AllocateObject.
    MirEffectTraits{.writesPlace = true,
                    .writesUnknownMemory = true,
                    .mayTrap = true,
                    .initializesValue = true,
                    .invokesUserCode = true}, // InitializeBase.
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true}, // CleanupBeforeAssign.
    MirEffectTraits{.readsPlace = true,
                    .mayTrap = true,
                    .dependsOnLoan = true}, // StorageConstructPreflight.
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
    harmless(), // FunctionItem
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
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .movesValue = true}, // UniqueOwnerSwap
    // UniqueOwnerUpcast
    MirEffectTraits{
        .readsPlace = true, .writesPlace = true, .movesValue = true},
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
    // PrefixStorageCString: a read-only owner-dependent NUL-terminated view.
    MirEffectTraits{.readsUnknownMemory = true, .startsLoan = true},
    // PrefixStorageTryCString: validates the logical prefix before producing
    // the same owner-dependent view, or a null value.
    MirEffectTraits{.readsUnknownMemory = true, .startsLoan = true},
    // OptionalStorageHasValue: a pure read of inline engagement state.
    MirEffectTraits{
        .readsPlace = true, .removableWhenUnused = true, .reorderable = true},
    // OptionalStorageConstruct
    MirEffectTraits{.writesPlace = true,
                    .mayTrap = true,
                    .initializesValue = true,
                    .invokesUserCode = true},
    // OptionalStorageRead
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    // OptionalStorageReadMut
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    // OptionalStorageReset
    MirEffectTraits{.writesPlace = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true},
    // PolymorphicStorageHasValue: a pure read of inline engagement state.
    MirEffectTraits{
        .readsPlace = true, .removableWhenUnused = true, .reorderable = true},
    // PolymorphicStorageConstruct: publishes one completed payload whose
    // cleanup is inert. A payload outside the native footprint traps.
    MirEffectTraits{
        .writesPlace = true, .mayTrap = true, .initializesValue = true},
    // PolymorphicStorageRead
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    // PolymorphicStorageReadMut
    MirEffectTraits{.readsPlace = true, .mayTrap = true, .startsLoan = true},
    // PolymorphicStorageReset: ends an inert payload lifetime.
    MirEffectTraits{.writesPlace = true, .dropsValue = true},
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
    MirEffectTraits{}, // ExactTypeParameterConstruction: scalar identity.
    MirEffectTraits{.mayTrap = true}, // RequireKey
    MirEffectTraits{.mayTrap = true}, // RequireContainerArgument
    // Panic snapshots a borrowed message into the explicit failure channel.
    MirEffectTraits{.readsUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true},          // Panic
    MirEffectTraits{.initializesValue = true}, // SharedOwnerEmpty
    // Promotion consumes the unique owner. Allocation rollback may execute
    // the pointee's source cleanup through the explicit failure channel.
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .initializesValue = true,
                    .dropsValue = true,
                    .invokesUserCode = true}, // SharedOwnerFromUnique
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .initializesValue = true,
                    .maySynchronize = true}, // SharedOwnerClone
    MirEffectTraits{.readsUnknownMemory = true,
                    .mayTrap = true,
                    .startsLoan = true}, // SharedOwnerBorrow
    MirEffectTraits{.readsPlace = true}, // SharedOwnerIsNull
    MirEffectTraits{.readsUnknownMemory = true,
                    .maySynchronize = true}, // SharedOwnerUseCount
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .movesValue = true}, // SharedOwnerSwap
    MirEffectTraits{.movesValue = true,
                    .initializesValue = true}, // SharedOwnerUpcast
    harmless(),                                // SourceFileName
    harmless(),                                // SourceFunctionName
    harmless(),                                // SourceLine
    harmless(),                                // SourceColumn
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdAdd
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdSubtract
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdMultiply
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdDivide
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdEqual
    MirEffectTraits{.readsPlace = true, .writesPlace = true}, // SimdLess
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .movesValue = true,
                    .initializesValue = true}, // RestoringExchange
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .movesValue = true}, // RestoringSwap
    MirEffectTraits{.readsPlace = true,
                    .writesPlace = true,
                    .readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .mayTrap = true,
                    .movesValue = true}, // RestoringIndexedSwap
    harmless(),                          // EnumUnderlyingValue
    MirEffectTraits{.readsUnknownMemory = true,
                    .startsLoan = true,
                    .dependsOnLoan = true}, // RawBorrow
    MirEffectTraits{.readsUnknownMemory = true,
                    .startsLoan = true,
                    .dependsOnLoan = true}, // RawBorrowMut
    harmless(), // RawPointerConversion: inert address, no pointee access.
    MirEffectTraits{.targetDependent = true,
                    .speculatable = true,
                    .removableWhenUnused = true,
                    .reorderable = true}, // ObjectSize
    MirEffectTraits{.targetDependent = true,
                    .speculatable = true,
                    .removableWhenUnused = true,
                    .reorderable = true}, // ObjectAlignment
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .mayTrap = true,
                    .dropsValue = true,
                    .invokesUserCode = true}, // ObjectDestroy
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .initializesValue = true,
                    .invokesUserCode = true}, // ObjectConstruct
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .movesValue = true,
                    .initializesValue = true}, // ObjectRelocate
    MirEffectTraits{.mayTrap = true},          // RequireVacantStorage
    MirEffectTraits{.mayTrap = true},          // RequireLiveStorage
    MirEffectTraits{.mayTrap = true},          // RequireRelocationCapacity
    MirEffectTraits{.mayTrap = true},          // RequireRelocationSource
    MirEffectTraits{.mayTrap = true},          // RequireRelocationDestination
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
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .maySynchronize = true}, // ConditionWait
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .maySynchronize = true}, // ConditionNotify
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
    "allocate-object",
    "initialize-base",
    "cleanup-before-assign",
    "storage-construct-preflight",
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
    "function-item",
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
    "unique-owner-swap",
    "unique-owner-upcast",
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
    "prefix-storage-c-string",
    "prefix-storage-try-c-string",
    "optional-storage-has-value",
    "optional-storage-construct",
    "optional-storage-read",
    "optional-storage-read-mut",
    "optional-storage-reset",
    "polymorphic-storage-has-value",
    "polymorphic-storage-construct",
    "polymorphic-storage-read",
    "polymorphic-storage-read-mut",
    "polymorphic-storage-reset",
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
    "exact-type-parameter-construction",
    "require-key",
    "require-container-argument",
    "panic",
    "shared-owner-empty",
    "shared-owner-from-unique",
    "shared-owner-clone",
    "shared-owner-borrow",
    "shared-owner-is-null",
    "shared-owner-use-count",
    "shared-owner-swap",
    "shared-owner-upcast",
    "source-file-name",
    "source-function-name",
    "source-line",
    "source-column",
    "simd-add",
    "simd-subtract",
    "simd-multiply",
    "simd-divide",
    "simd-equal",
    "simd-less",
    "restoring-exchange",
    "restoring-swap",
    "restoring-indexed-swap",
    "enum-underlying-value",
    "raw-borrow",
    "raw-borrow-mut",
    "raw-pointer-conversion",
    "object-size",
    "object-alignment",
    "object-destroy",
    "object-construct",
    "object-relocate",
    "require-vacant-storage",
    "require-live-storage",
    "require-relocation-capacity",
    "require-relocation-source",
    "require-relocation-destination",
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
    "condition-wait",
    "condition-notify",
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
  result = merge(result, mir_detail::rawPointerConversion(instruction)
                             ? harmless()
                             : effects(instruction.operation));
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
      instruction.operation == MirOperation::AddressOf) {
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
      result.dropsValue || result.invokesUserCode ||
      (result.targetDependent &&
       instruction.intrinsic != IntrinsicKind::ObjectSize &&
       instruction.intrinsic != IntrinsicKind::ObjectAlignment) ||
      result.maySynchronize;
  if (observable) {
    result.speculatable = false;
    result.removableWhenUnused = false;
    result.reorderable = false;
  }
  return result;
}

[[nodiscard]] MirEffectTraits
exactInvocationShell(const MirInstruction &instruction,
                     bool nativeCleanup = false) {
  MirEffectTraits result;
  switch (instruction.kind) {
  case MirInstructionKind::Call:
  case MirInstructionKind::CallBody:
    result.invokesUserCode = true;
    break;
  case MirInstructionKind::Construct:
  case MirInstructionKind::InitializeBase:
    result.writesPlace = instruction.kind == MirInstructionKind::InitializeBase;
    result.initializesValue = true;
    result.invokesUserCode = true;
    break;
  case MirInstructionKind::Drop:
  case MirInstructionKind::CleanupBeforeAssign:
    result.readsPlace =
        instruction.kind == MirInstructionKind::CleanupBeforeAssign;
    result.writesPlace = true;
    result.invokesRuntime = nativeCleanup;
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
  bool nativeCleanup = false;
  bool synchronizingCleanup = false;
  std::vector<MirBodyAddress> addresses;
};

[[nodiscard]] bool sharedCleanup(const MirProgram &program,
                                 const MirDropType &drop) {
  if (drop.classInstance) {
    const auto *owner = program.findClassInstance(*drop.classInstance);
    if (owner &&
        owner->compilerCapability == CompilerCapabilityTypeKind::SharedOwner)
      return true;
  }
  return std::any_of(
      drop.aggregate.begin(), drop.aggregate.end(),
      [&](const auto &child) { return sharedCleanup(program, child); });
}

[[nodiscard]] MirEffectTraits cleanupEffects(MirEffectTraits traits,
                                             const ExactTargets &targets) {
  if (targets.synchronizingCleanup) {
    traits.readsUnknownMemory = true;
    traits.writesUnknownMemory = true;
    traits.maySynchronize = true;
    traits.speculatable = false;
    traits.removableWhenUnused = false;
    traits.reorderable = false;
  }
  return traits;
}

[[nodiscard]] ExactTargets exactTargets(const MirProgram &program,
                                        const MirBody &body,
                                        const MirInstruction &instruction) {
  ExactTargets result;
  if (instruction.kind == MirInstructionKind::CleanupBeforeAssign &&
      instruction.replacementCleanup) {
    result.nativeCleanup = true;
    result.synchronizingCleanup =
        sharedCleanup(program, *instruction.replacementCleanup);
    result.boundary =
        mirDropHasPolymorphicCleanup(*instruction.replacementCleanup);
    for (const auto destructor :
         mirDropDestructorTargets(*instruction.replacementCleanup)) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Destructor, .owner = destructor});
    }
  } else if (instruction.kind == MirInstructionKind::Call &&
             instruction.elementCleanup) {
    result.nativeCleanup = true;
    result.synchronizingCleanup =
        sharedCleanup(program, *instruction.elementCleanup);
    result.boundary = mirDropHasPolymorphicCleanup(*instruction.elementCleanup);
    for (const auto destructor :
         mirDropDestructorTargets(*instruction.elementCleanup)) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Destructor,
           .owner = static_cast<std::size_t>(destructor)});
    }
  } else if (instruction.kind == MirInstructionKind::Call &&
             (instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
              instruction.intrinsic == IntrinsicKind::PrefixStorageAppend) &&
             instruction.constructorTarget &&
             (instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
              instruction.storagePreflight != 0)) {
    // The sealed slot operation also executes the exact source constructor;
    // retain its memory, synchronization and escape effects in the shared
    // interprocedural analysis rather than treating it as only a helper.
    result.addresses.push_back(
        {.kind = MirBodyKind::Constructor,
         .owner = static_cast<std::size_t>(*instruction.constructorTarget)});
  } else if (instruction.kind == MirInstructionKind::Call &&
             (instruction.intrinsic != IntrinsicKind::None ||
              instruction.synchronization.kind !=
                  SynchronizationOperationKind::None)) {
    return result;
  } else if (instruction.kind == MirInstructionKind::Call) {
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
  } else if (instruction.kind == MirInstructionKind::Construct ||
             instruction.kind == MirInstructionKind::InitializeBase) {
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
      result.nativeCleanup |=
          obligation.dropType.uniquePointeeClass.has_value() ||
          !obligation.dropType.aggregate.empty();
      result.synchronizingCleanup |=
          sharedCleanup(program, obligation.dropType);
      result.boundary |= mirDropHasPolymorphicCleanup(obligation.dropType);
      for (const auto destructor :
           mirDropDestructorTargets(obligation.dropType)) {
        result.addresses.push_back(
            {.kind = MirBodyKind::Destructor,
             .owner = static_cast<std::size_t>(destructor)});
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
         instruction.kind == MirInstructionKind::InitializeBase ||
         instruction.kind == MirInstructionKind::Drop ||
         instruction.kind == MirInstructionKind::CleanupBeforeAssign ||
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

std::optional<MirDropType> mirUniqueCleanupType(const MirProgram &program,
                                                const SemanticType &type) {
  std::unordered_set<HirClassInstanceId> visiting;
  const auto classFor =
      [&](const SemanticType &selected) -> const MirClassInstance * {
    for (const auto &candidate : program.classInstances())
      if (candidate.type == selected)
        return &candidate;
    return nullptr;
  };
  const auto derive =
      [&](const auto &self,
          const SemanticType &selected) -> std::optional<MirDropType> {
    if (selected.kind == SemanticType::Array) {
      if (selected.arguments.size() != 1 || selected.arrayLength == 0 ||
          selected.arrayLengthParameterId != 0)
        return std::nullopt;
      auto child = self(self, selected.arguments.front());
      if (!child || !child->requiresActiveCleanup)
        return std::nullopt;
      SemanticType leaf = selected;
      MirFixedArrayDropSchedule schedule;
      while (leaf.kind == SemanticType::Array) {
        if (leaf.arguments.size() != 1 || leaf.arrayLength == 0 ||
            leaf.arrayLengthParameterId != 0)
          return std::nullopt;
        schedule.extents.push_back(leaf.arrayLength);
        leaf = SemanticType(leaf.arguments.front());
      }
      schedule.leafType = leaf;
      const auto *owner =
          leaf.kind == SemanticType::Class ? classFor(leaf) : nullptr;
      if (owner)
        schedule.leafClassInstance = owner->id;
      return MirDropType{.type = selected,
                         .destructor = owner ? owner->destructor : std::nullopt,
                         .fixedArray = std::move(schedule),
                         .aggregate = {*child},
                         .requiresActiveCleanup = true};
    }
    if (const auto *owner =
            selected.kind == SemanticType::Class ? classFor(selected) : nullptr;
        owner != nullptr &&
        owner->compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
      if (selected.arguments.size() != 1)
        return std::nullopt;
      // Each token releases one strong reference. Only the final release
      // executes this exact existing allocation/pointee cleanup recipe.
      auto pointee =
          self(self, SemanticType::uniqueOwnerOf(selected.arguments.front()));
      if (!pointee)
        return std::nullopt;
      return MirDropType{.type = selected,
                         .classInstance = owner->id,
                         .aggregate = {*pointee},
                         .requiresActiveCleanup = true};
    }
    if (const auto *owner =
            selected.kind == SemanticType::Class ? classFor(selected) : nullptr;
        owner != nullptr && (owner->compilerCapability ==
                                 CompilerCapabilityTypeKind::MutexStorage ||
                             owner->compilerCapability ==
                                 CompilerCapabilityTypeKind::OptionalStorage)) {
      if (selected.arguments.size() != 1 || !owner->requiresActiveCleanup)
        return std::nullopt;
      auto payload = self(self, selected.arguments.front());
      if (!payload || !payload->requiresActiveCleanup)
        return std::nullopt;
      // These private inline capabilities own a conditionally live payload
      // rather than a source-visible field. Preserve that
      // ownership edge explicitly so an enclosing aggregate or shared owner
      // can discharge the payload before leaving the native shell empty.
      return MirDropType{.type = selected,
                         .classInstance = owner->id,
                         .aggregate = {*payload},
                         .requiresActiveCleanup = true};
    }
    if (selected.kind == SemanticType::Enum) {
      const auto *enumeration = program.findPayloadEnum(selected);
      if (enumeration == nullptr)
        return std::nullopt;
      MirDropType result{.type = selected,
                         .requiresActiveCleanup =
                             enumeration->requiresActiveCleanup};
      for (const auto &variant : enumeration->variants) {
        if (variant.payloadTypes.size() !=
            variant.payloadRequiresActiveCleanup.size())
          return std::nullopt;
        for (std::size_t index = variant.payloadTypes.size(); index != 0;
             --index) {
          if (!variant.payloadRequiresActiveCleanup[index - 1])
            continue;
          auto child = self(self, variant.payloadTypes[index - 1]);
          if (!child)
            return std::nullopt;
          child->payloadVariant = variant.variantIndex;
          child->payloadField = index - 1;
          result.aggregate.push_back(*child);
        }
      }
      return result;
    }
    if ((selected.kind == SemanticType::Expected &&
         selected.arguments.size() == 2) ||
        (selected.kind == SemanticType::Unexpected &&
         selected.arguments.size() == 1)) {
      MirDropType result{.type = selected};
      for (const auto &alternative : selected.arguments) {
        auto child = self(self, alternative);
        if (!child) {
          const bool passive =
              alternative == SemanticType::Void ||
              (alternative.kind >= SemanticType::Int8 &&
               alternative.kind <= SemanticType::Char) ||
              alternative.kind == SemanticType::RawPointer ||
              alternative.kind == SemanticType::NativeFunction ||
              alternative.kind == SemanticType::NullPtr ||
              alternative.kind == SemanticType::CString ||
              alternative.kind == SemanticType::StringView ||
              (alternative.kind == SemanticType::Enum &&
               program.findPayloadEnum(alternative) == nullptr);
          if (passive) {
            child = MirDropType{.type = alternative};
          } else {
            const auto *owner = alternative.kind == SemanticType::Class
                                    ? classFor(alternative)
                                    : nullptr;
            if (owner == nullptr || owner->requiresActiveCleanup)
              return std::nullopt;
            child =
                MirDropType{.type = alternative, .classInstance = owner->id};
          }
        }
        result.requiresActiveCleanup |= child->requiresActiveCleanup;
        result.aggregate.push_back(*child);
      }
      return result;
    }
    if (selected.kind == SemanticType::Lambda) {
      const auto lambda = std::find_if(program.lambdaInstances().begin(),
                                       program.lambdaInstances().end(),
                                       [&](const MirLambdaInstance &candidate) {
                                         return candidate.type == selected;
                                       });
      if (lambda == program.lambdaInstances().end() ||
          lambda->captureTypes.size() !=
              lambda->captureRequiresActiveCleanup.size())
        return std::nullopt;
      const auto exactCaptures = selected.lambdaCaptureTypes();
      if (exactCaptures.size() != lambda->captureTypes.size() ||
          !std::equal(exactCaptures.begin(), exactCaptures.end(),
                      lambda->captureTypes.begin()))
        return std::nullopt;
      MirDropType result{.type = selected, .lambdaInstance = lambda->id};
      for (std::size_t index = lambda->captureTypes.size(); index != 0;
           --index) {
        if (!lambda->captureRequiresActiveCleanup[index - 1])
          continue;
        auto child = self(self, lambda->captureTypes[index - 1]);
        if (!child)
          return std::nullopt;
        child->captureIndex = index - 1;
        result.aggregate.push_back(*child);
      }
      result.requiresActiveCleanup = !result.aggregate.empty();
      return result;
    }
    if ((selected.kind == SemanticType::UniqueOwner ||
         selected.kind == SemanticType::PrefixStorage) &&
        selected.arguments.size() == 1) {
      const auto &value = selected.arguments.front();
      if ((value.kind >= SemanticType::Int8 &&
           value.kind <= SemanticType::Char) ||
          value.kind == SemanticType::RawPointer ||
          value.kind == SemanticType::NativeFunction ||
          value.kind == SemanticType::NullPtr ||
          value.kind == SemanticType::CString ||
          value.kind == SemanticType::StringView ||
          (value.kind == SemanticType::Enum &&
           program.findPayloadEnum(value) == nullptr)) {
        return MirDropType{.type = selected, .requiresActiveCleanup = true};
      }
      if (selected.kind == SemanticType::PrefixStorage) {
        auto element = self(self, value);
        if (!element)
          return std::nullopt;
        return MirDropType{.type = selected,
                           .aggregate = {*element},
                           .requiresActiveCleanup = true};
      }
      const auto *pointee = classFor(selected.arguments.front());
      if (pointee == nullptr)
        return std::nullopt;
      if (pointee->polymorphic)
        return MirDropType{.type = selected,
                           .uniquePointeeClass = pointee->id,
                           .polymorphicCleanup = true,
                           .requiresActiveCleanup = true};
      if (!pointee->requiresActiveCleanup)
        return MirDropType{.type = selected,
                           .uniquePointeeClass = pointee->id,
                           .requiresActiveCleanup = true};
      if (pointee->destructor && pointee->requiresActiveDropState &&
          pointee->bases.empty() &&
          std::none_of(
              pointee->declaredFields.begin(), pointee->declaredFields.end(),
              [](const auto &field) { return field.requiresActiveCleanup; })) {
        return MirDropType{.type = selected,
                           .destructor = pointee->destructor,
                           .uniquePointeeClass = pointee->id,
                           .requiresActiveCleanup = true};
      }
      auto child = self(self, pointee->type);
      if (!child)
        return std::nullopt;
      return MirDropType{.type = selected,
                         .uniquePointeeClass = pointee->id,
                         .aggregate = {*child},
                         .requiresActiveCleanup = true};
    }
    const auto *owner =
        selected.kind == SemanticType::Class ? classFor(selected) : nullptr;
    if (owner != nullptr && owner->destructor &&
        owner->requiresActiveDropState && owner->bases.empty() &&
        std::none_of(
            owner->declaredFields.begin(), owner->declaredFields.end(),
            [](const auto &field) { return field.requiresActiveCleanup; })) {
      return MirDropType{.type = selected,
                         .classInstance = owner->id,
                         .destructor = owner->destructor,
                         .requiresActiveCleanup = true};
    }
    if (owner == nullptr || owner->unionLayout ||
        (owner->destructor
             ? (!owner->requiresActiveDropState ||
                owner->destructorStatus != SpecialMemberStatus::Declared)
             : (owner->requiresActiveDropState ||
                owner->destructorStatus != SpecialMemberStatus::Generated)))
      return std::nullopt;
    if (!visiting.insert(owner->id).second)
      return MirDropType{.type = selected,
                         .classInstance = owner->id,
                         .recursiveCleanup = true,
                         .requiresActiveCleanup = true};
    MirDropType result{.type = selected,
                       .classInstance = owner->id,
                       .destructor = owner->destructor,
                       .requiresActiveCleanup = true};
    bool complete = true;
    for (auto cursor = owner->declaredFields.rbegin();
         cursor != owner->declaredFields.rend(); ++cursor) {
      const auto &field = *cursor;
      if (!field.requiresActiveCleanup)
        continue;
      auto child = self(self, field.type);
      if (child) {
        child->uniqueOwnerField = field.symbol;
        result.aggregate.push_back(*child);
      } else {
        complete = false;
      }
    }
    for (const auto &baseInfo : owner->bases) {
      const auto *base = program.findClassInstance(baseInfo.instance);
      if (base == nullptr) {
        complete = false;
        continue;
      }
      if (!base->requiresActiveCleanup)
        continue;
      auto child = self(self, base->type);
      if (child) {
        child->uniqueOwnerBase = base->id;
        result.aggregate.push_back(*child);
      } else {
        complete = false;
      }
    }
    visiting.erase(owner->id);
    if (!complete || (!result.destructor && result.aggregate.empty()))
      return std::nullopt;
    return result;
  };
  return derive(derive, type);
}

std::vector<HirDestructorInstanceId>
mirDropDestructorTargets(const MirDropType &type) {
  std::vector<HirDestructorInstanceId> result;
  const auto collect = [&](const auto &self, const MirDropType &step) -> void {
    if (step.destructor)
      result.push_back(*step.destructor);
    for (const auto &child : step.aggregate)
      self(self, child);
  };
  collect(collect, type);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

RelocationKind mirTypeRelocationKind(const MirProgram &program,
                                     const SemanticType &type) {
  std::unordered_set<HirClassInstanceId> visiting;
  // One declaration family may occur repeatedly along a finite layout path
  // with distinct arguments. Only revisiting the same exact type is a cycle.
  std::vector<SemanticType> enums;
  const auto derive = [&](const auto &self,
                          const SemanticType &candidate) -> RelocationKind {
    const auto merge = [](RelocationKind left, RelocationKind right) {
      return left == RelocationKind::Unavailable ||
                     right == RelocationKind::Unavailable
                 ? RelocationKind::Unavailable
             : left == RelocationKind::Bitwise &&
                     right == RelocationKind::Bitwise
                 ? RelocationKind::Bitwise
                 : RelocationKind::Structural;
    };
    if (candidate.kind == SemanticType::Array)
      return candidate.arguments.size() == 1 &&
                     candidate.arrayLengthParameterId == 0
                 ? self(self, candidate.arguments.front())
                 : RelocationKind::Unavailable;
    if (candidate.kind == SemanticType::Expected ||
        candidate.kind == SemanticType::Unexpected ||
        candidate.kind == SemanticType::Lambda) {
      if ((candidate.kind == SemanticType::Expected &&
           candidate.arguments.size() != 2) ||
          (candidate.kind == SemanticType::Unexpected &&
           candidate.arguments.size() != 1) ||
          (candidate.kind == SemanticType::Lambda &&
           !candidate.hasLambdaShape()))
        return RelocationKind::Unavailable;
      const auto parts =
          candidate.kind == SemanticType::Lambda
              ? candidate.lambdaCaptureTypes()
              : std::span<const SemanticType>{candidate.arguments};
      // A closure environment is a plain aggregate of its captures; an
      // expected keeps its engagement state and stays structural.
      auto result = candidate.kind == SemanticType::Lambda
                        ? RelocationKind::Bitwise
                        : RelocationKind::Structural;
      for (const auto &part : parts) {
        if (candidate.kind == SemanticType::Expected &&
            &part == &candidate.arguments.front() && part == SemanticType::Void)
          continue;
        result = merge(result, self(self, part));
      }
      return result;
    }
    if (candidate.kind == SemanticType::Enum) {
      if (const auto *enumeration = program.findPayloadEnum(candidate)) {
        if (std::find(enums.begin(), enums.end(), candidate) != enums.end())
          return RelocationKind::Unavailable;
        enums.push_back(candidate);
        auto result = RelocationKind::Structural;
        for (const auto &variant : enumeration->variants)
          for (const auto &payload : variant.payloadTypes)
            result = merge(result, self(self, payload));
        enums.pop_back();
        return result;
      }
    }
    if (candidate.kind != SemanticType::Class)
      return semanticTraits(candidate).relocation;
    const MirClassInstance *owner = nullptr;
    for (const auto &instance : program.classInstances()) {
      if (instance.type != candidate)
        continue;
      if (owner != nullptr)
        return RelocationKind::Unavailable;
      owner = &instance;
    }
    if (owner == nullptr || !visiting.insert(owner->id).second)
      return RelocationKind::Unavailable;
    auto result = RelocationKind::Bitwise;
    if (owner->compilerCapability == CompilerCapabilityTypeKind::TextView ||
        owner->compilerCapability == CompilerCapabilityTypeKind::CString) {
      result = RelocationKind::Bitwise;
    } else if (owner->compilerCapability ==
                   CompilerCapabilityTypeKind::SharedOwner ||
               owner->compilerCapability ==
                   CompilerCapabilityTypeKind::PolymorphicStorage) {
      // Polymorphic storage relocates its payload through the payload's
      // generated move; it is never a byte copy of a polymorphic object.
      result = RelocationKind::Structural;
    } else if (owner->compilerCapability ==
               CompilerCapabilityTypeKind::OptionalStorage) {
      result = candidate.arguments.size() == 1
                   ? merge(RelocationKind::Structural,
                           self(self, candidate.arguments.front()))
                   : RelocationKind::Unavailable;
    } else if (owner->compilerCapability ==
               CompilerCapabilityTypeKind::UninitializedStorage) {
      result = candidate.arguments.size() == 1 &&
                       self(self, candidate.arguments.front()) ==
                           RelocationKind::Bitwise
                   ? RelocationKind::Bitwise
                   : RelocationKind::Unavailable;
    } else if (owner->compilerCapability != CompilerCapabilityTypeKind::None ||
               owner->moveConstructor != SpecialMemberStatus::Generated ||
               owner->abstract || owner->cOpaqueHandle ||
               owner->lifecycleBoundBorrowCarrier ||
               owner->traits.containsBorrowedState) {
      result = RelocationKind::Unavailable;
    } else {
      if (owner->destructor || owner->polymorphic || !owner->bases.empty() ||
          owner->unionLayout)
        result = RelocationKind::Structural;
      for (const auto &field : owner->declaredFields)
        result = merge(result, self(self, field.type));
      for (const auto &base : owner->bases) {
        // Interface bases carry dispatch requirements, not stored value state.
        if (base.interface)
          continue;
        const auto *instance = program.findClassInstance(base.instance);
        result = merge(result, instance ? self(self, instance->type)
                                        : RelocationKind::Unavailable);
      }
    }
    visiting.erase(owner->id);
    return result;
  };
  return derive(derive, type);
}

bool mirTypeRelocationIsDefinedFailureFree(const MirProgram &program,
                                           const SemanticType &type) {
  std::unordered_set<HirClassInstanceId> classes;
  std::unordered_set<HirLambdaId> lambdas;
  const auto prove = [&](const auto &self,
                         const SemanticType &candidate) -> bool {
    if (mirTypeMoveIsDefinedFailureFree(program, candidate))
      return true;
    if (candidate.kind == SemanticType::Array) {
      return candidate.arguments.size() == 1 &&
             candidate.arrayLengthParameterId == 0 &&
             self(self, candidate.arguments.front());
    }
    if (candidate.kind == SemanticType::Expected ||
        candidate.kind == SemanticType::Unexpected) {
      const std::size_t count =
          candidate.kind == SemanticType::Expected ? 2 : 1;
      return candidate.arguments.size() == count &&
             std::all_of(candidate.arguments.begin(), candidate.arguments.end(),
                         [&](const auto &value) {
                           return value == SemanticType::Void ||
                                  self(self, value);
                         });
    }
    if (candidate.kind == SemanticType::Lambda) {
      const MirLambdaInstance *lambda = nullptr;
      for (const auto &instance : program.lambdaInstances()) {
        if (instance.type != candidate)
          continue;
        if (lambda != nullptr)
          return false;
        lambda = &instance;
      }
      if (lambda == nullptr || !lambdas.insert(lambda->id).second)
        return false;
      const bool captures =
          std::all_of(lambda->captureTypes.begin(), lambda->captureTypes.end(),
                      [&](const auto &capture) { return self(self, capture); });
      lambdas.erase(lambda->id);
      return captures;
    }
    if (candidate.kind != SemanticType::Class)
      return false;
    const MirClassInstance *owner = nullptr;
    for (const auto &instance : program.classInstances()) {
      if (instance.type != candidate)
        continue;
      if (owner != nullptr)
        return false;
      owner = &instance;
    }
    if (owner == nullptr ||
        owner->moveConstructor != SpecialMemberStatus::Generated ||
        owner->unionLayout || !classes.insert(owner->id).second)
      return false;
    if (owner->compilerCapability != CompilerCapabilityTypeKind::None) {
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::UninitializedStorage) {
        const bool safe =
            candidate.arguments.size() == 1 &&
            mirTypeRelocationKind(program, candidate.arguments.front()) ==
                RelocationKind::Bitwise;
        classes.erase(owner->id);
        return safe;
      }
      // Every admitted polymorphic payload has failure-free relocation.
      const bool safe =
          owner->compilerCapability ==
              CompilerCapabilityTypeKind::PolymorphicStorage ||
          (owner->compilerCapability ==
               CompilerCapabilityTypeKind::OptionalStorage &&
           candidate.arguments.size() == 1 &&
           self(self, candidate.arguments.front()));
      classes.erase(owner->id);
      return safe;
    }
    // Moving a guarded object transfers its active state, never calls its
    // source destructor. Native teardown of each moved-from subobject sees
    // only the recursively transferred representation.
    const bool destructor =
        owner->destructor
            ? owner->requiresActiveDropState &&
                  owner->destructorStatus == SpecialMemberStatus::Declared
            : !owner->requiresActiveDropState &&
                  owner->destructorStatus == SpecialMemberStatus::Generated;
    const bool fields =
        std::all_of(owner->declaredFields.begin(), owner->declaredFields.end(),
                    [&](const auto &field) {
                      return field.field != 0 && field.symbol != 0 &&
                             self(self, field.type);
                    });
    const bool bases = std::all_of(
        owner->bases.begin(), owner->bases.end(), [&](const auto &base) {
          const auto *instance = program.findClassInstance(base.instance);
          return instance != nullptr && self(self, instance->type);
        });
    classes.erase(owner->id);
    return destructor && fields && bases;
  };
  return prove(prove, type);
}

bool mirDropHasPolymorphicCleanup(const MirDropType &type) {
  return type.polymorphicCleanup ||
         std::any_of(type.aggregate.begin(), type.aggregate.end(),
                     mirDropHasPolymorphicCleanup);
}

bool mirDropMayRaiseDefinedFailure(const MirProgram &program,
                                   const MirDropType &type) {
  if (mirDropHasPolymorphicCleanup(type))
    return true;
  for (const auto id : mirDropDestructorTargets(type)) {
    const auto *target = program.findDestructorInstance(id);
    if (target == nullptr || target->mayRaiseDefinedFailure)
      return true;
  }
  return false;
}

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
  return effects(program, *body, instruction);
}

MirEffectTraits effects(const MirProgram &program, const MirBody &body,
                        const MirInstruction &instruction) {
  if (!isInvocation(instruction)) {
    return effects(instruction);
  }
  const ExactTargets targets = exactTargets(program, body, instruction);
  if (targets.boundary || targets.addresses.empty()) {
    return cleanupEffects(effects(instruction), targets);
  }
  MirEffectTraits result =
      exactInvocationShell(instruction, targets.nativeCleanup);
  for (const MirBodyAddress target : targets.addresses) {
    result = merge(result, summaryForAddress(program, target)->traits);
  }
  return cleanupEffects(result, targets);
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
  if (result && targets.nativeCleanup)
    result->traits.invokesRuntime = true;
  if (result && instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic == IntrinsicKind::PrefixStorageAppend) {
    // This invocation is the complete slot operation, not just its selected
    // constructor. The constructor receiver denotes the new element, whereas
    // the instruction argument denotes its backing storage. Retain the
    // authoritative intrinsic effects and conservatively map any escape to
    // that storage rather than exposing the constructor's receiver ABI.
    result->traits = merge(result->traits, effects(instruction));
    result->receiverEscapes = false;
    result->argumentsEscape.assign(instruction.operands.size(), true);
  }
  if (result)
    result->traits = cleanupEffects(result->traits, targets);
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
            function->returnBorrowParameter <
                node.local.argumentsEscape.size()) {
          node.local.argumentsEscape[function->returnBorrowParameter] = true;
          for (const auto parameter : function->returnBorrowParameters) {
            if (parameter < node.local.argumentsEscape.size()) {
              node.local.argumentsEscape[parameter] = true;
            }
          }
        }
      }
    } else if (node.address.kind == MirBodyKind::Constructor) {
      const MirConstructorInstance *constructor =
          program.findConstructorInstance(node.address.owner);
      if (constructor != nullptr) {
        node.local.receiverEscapes =
            constructor->borrowOrigin == BorrowOriginKind::Receiver;
        if (constructor->borrowOrigin == BorrowOriginKind::Argument &&
            constructor->borrowParameter < node.local.argumentsEscape.size()) {
          node.local.argumentsEscape[constructor->borrowParameter] = true;
          for (const auto parameter : constructor->borrowParameters) {
            if (parameter < node.local.argumentsEscape.size()) {
              node.local.argumentsEscape[parameter] = true;
            }
          }
        }
      }
    }

    for (const MirLoan &loan : node.body->loans) {
      if (!loan.escapes) {
        continue;
      }
      const auto origins = mirLoanProtectedSources(*node.body, loan.id);
      for (const MirPlaceId origin : origins) {
        const MirPlace *sourcePlace = node.body->findPlace(origin);
        if (sourcePlace == nullptr) {
          continue;
        }
        const MirPlace &source = *sourcePlace;
        if (source.root == MirPlaceRootKind::This) {
          node.local.receiverEscapes = true;
        }
        const auto parameter =
            std::find_if(node.body->places.begin(), node.body->places.end(),
                         [&](const MirPlace &place) {
                           return place.id == origin && place.binding != 0;
                         });
        if (parameter == node.body->places.end()) {
          continue;
        }
        const auto markParameter = [&](const auto &instance) {
          const auto found =
              std::find(instance.parameterBindings.begin(),
                        instance.parameterBindings.end(), parameter->binding);
          if (found != instance.parameterBindings.end()) {
            node.local.argumentsEscape[static_cast<std::size_t>(std::distance(
                instance.parameterBindings.begin(), found))] = true;
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
    }

    for (const MirBlock &block : node.body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const ExactTargets targets =
            exactTargets(program, *node.body, instruction);
        if (isInvocation(instruction) && !targets.addresses.empty() &&
            !targets.boundary) {
          node.local.traits =
              merge(node.local.traits,
                    cleanupEffects(exactInvocationShell(instruction,
                                                        targets.nativeCleanup),
                                   targets));
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
          const MirEffectTraits instructionEffects =
              cleanupEffects(effects(instruction), targets);
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
