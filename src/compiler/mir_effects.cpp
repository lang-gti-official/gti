#include "gti/mir_effects.h"
#include "gti/checked_integer.h"
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
    MirEffectTraits{.dependsOnLoan = true},   // ParkRangeCursor.
    MirEffectTraits{.dependsOnLoan = true},   // RestoreRangeCursor.
    unknownEffects(), // EnsureThreadLocal invokes the exact initializer once.
    unknownEffects(), // DrainThreadLocals runs exact initialized TLS cleanup.
    unknownEffects(), // BeginThreadLocals attaches the hosted thread lifetime.
});

constexpr auto operationEffects = std::to_array<MirEffectTraits>({
    harmless(), // None is neutral when an instruction has no scalar operation.
    harmless(), // Literal
    harmless(), // EnumConstant
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    trapping(), // Index
    harmless(), // Identity
    MirEffectTraits{.mayTrap = true, .targetDependent = true},
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    // Without a program row, conservatively retain a possible thread binding.
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .mayTrap = true}, // NativeCallback
    harmless(),                       // FunctionItem
    MirEffectTraits{.copiesValue = true, .initializesValue = true},
    MirEffectTraits{.copiesValue = true},     // PayloadExtract
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
    harmless(), // IntegerAddOverflows
    harmless(), // IntegerSubtractOverflows
    harmless(), // IntegerMultiplyOverflows
    // StorageBoundsCheck: a pure comparison over two staged scalars whose
    // only effect is the defined-failure trap on an out-of-bounds index.
    MirEffectTraits{.mayTrap = true},
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
    // Erase/restore checked references retain ordinary loan authority.
    MirEffectTraits{.startsLoan = true, .dependsOnLoan = true},
    MirEffectTraits{.startsLoan = true, .dependsOnLoan = true},
    MirEffectTraits{.startsLoan = true, .dependsOnLoan = true},
    MirEffectTraits{.startsLoan = true, .dependsOnLoan = true},
    // Stateless target formation has no runtime environment.
    MirEffectTraits{.initializesValue = true},
    // Signature erasure hides target effects but preserves its failure ABI.
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .mayTrap = true,
                    .invokesUserCode = true},
    // StringViewSize: a pure read of the view's stored length.
    MirEffectTraits{},
    // StringViewEmpty
    MirEffectTraits{},
    // ArrayRangeBegin: a pure compiler-generated zero endpoint.
    MirEffectTraits{},
    // ArraySize: a pure read of the fixed extent.
    MirEffectTraits{},
    // ThreadHandleEmpty: creates an empty private capability value.
    MirEffectTraits{.initializesValue = true},
    // ThreadSpawn: synchronization metadata supplies the full barrier facts.
    MirEffectTraits{.movesValue = true, .initializesValue = true},
    // ThreadJoinable: reads the opaque handle state without synchronizing.
    MirEffectTraits{.readsUnknownMemory = true},
    // ThreadJoin: synchronization metadata supplies the join barrier.
    MirEffectTraits{},
    // ThreadResultTake: moves the joined result out of the handle.
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .movesValue = true,
                    .initializesValue = true},
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
    // AtomicWait/Notify: synchronization metadata preserves blocking/wakeup.
    MirEffectTraits{},
    MirEffectTraits{},
    MirEffectTraits{},
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
    MirEffectTraits{.mayTrap = true},          // FailEmptyOwner
    MirEffectTraits{.mayTrap = true},          // FailInactiveValue
    MirEffectTraits{.mayTrap = true},          // FailInactiveError
    MirEffectTraits{.initializesValue = true}, // SharedOwnerEmpty
    // Promotion consumes the unique owner. Exhaustion aborts before cleanup;
    // successful publication does not execute the pointee's source body.
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .movesValue = true,
                    .initializesValue = true}, // SharedOwnerFromUnique
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
    harmless(), // SourceExpressionText: no executable form; verifier rejects.
    harmless(), // MetaFieldCount: folded in semantics, never executes.
    harmless(), // MetaFieldName
    harmless(), // MetaFieldIsPublic
    harmless(), // MetaFieldHasDefault
    harmless(), // MetaTypeName
    harmless(), // MetaTypeKind
    harmless(), // MetaFieldAttributeCount
    harmless(), // MetaFieldHasAttribute
    harmless(), // MetaTypeAttributeCount
    harmless(), // MetaTypeHasAttribute
    harmless(), // MetaTypeTrait
    harmless(), // MetaTypeRelation
    harmless(), // MetaEnumeratorCount
    harmless(), // MetaEnumeratorName
    harmless(), // MetaEnumeratorValue
    harmless(), // MetaPayloadCount
    harmless(), // MetaPayloadName
    harmless(), // MetaBaseCount
    harmless(), // MetaArgumentCount
    harmless(), // MetaMethodCount
    harmless(), // MetaMethodName
    harmless(), // MetaMethodTrait
    harmless(), // MetaMethodParameterCount
    harmless(), // MetaParameterName
    harmless(), // MetaConstructorCount
    harmless(), // MetaConstructorParameterCount
    harmless(), // MetaConstructorParameterName
    harmless(), // MetaDeclarationLine
    harmless(), // MetaDeclarationFile
    harmless(), // MetaTypePrimaryName
    harmless(), // MetaFieldAttribute: aliased to a struct construction.
    harmless(), // MetaTypeAttribute
    harmless(), // MetaFieldDefault: aliased to the declared initializer.
    harmless(), // MetaFieldGet: aliased to the field projection it denotes.
    harmless(), // MetaFieldGetMut
    harmless(), // MetaConstruct: recorded as the designated construction.
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
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .movesValue = true,
                    .initializesValue = true}, // ObjectRelocateRange
    MirEffectTraits{.mayTrap = true},          // RequireVacantStorage
    MirEffectTraits{.mayTrap = true},          // RequireLiveStorage
    MirEffectTraits{.mayTrap = true},          // RequireRelocationCapacity
    MirEffectTraits{.mayTrap = true},          // RequireRelocationSource
    MirEffectTraits{.mayTrap = true},          // RequireRelocationDestination
    MirEffectTraits{.startsLoan = true,
                    .dependsOnLoan = true}, // TextViewFromAddress
    MirEffectTraits{.startsLoan = true,
                    .dependsOnLoan = true}, // CStringFromAddress
    MirEffectTraits{.mayTrap = true},       // RequireAllocation
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dependsOnLoan = true,
                    .movesValue = true,
                    .invokesUserCode = true,
                    .maySynchronize = true}, // ThreadScope
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
                    .invokesRuntime = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicWait
    MirEffectTraits{.writesUnknownMemory = true,
                    .invokesRuntime = true,
                    .targetDependent = true,
                    .maySynchronize = true}, // AtomicNotify
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
    MirEffectTraits{.readsUnknownMemory = true,
                    .writesUnknownMemory = true,
                    .allocates = true,
                    .invokesRuntime = true,
                    .mayTrap = true,
                    .dependsOnLoan = true,
                    .movesValue = true,
                    .invokesUserCode = true,
                    .maySynchronize = true}, // ThreadScope
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
    "park-range-cursor",
    "restore-range-cursor",
    "ensure-thread-local",
    "drain-thread-locals",
    "begin-thread-locals",
});

constexpr auto operationNames = std::to_array<std::string_view>({
    "none",
    "literal",
    "enum-constant",
    "aggregate",
    "index",
    "identity",
    "convert",
    "closure",
    "native-callback",
    "function-item",
    "payload-construct",
    "payload-extract",
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
    "integer-add-overflows",
    "integer-subtract-overflows",
    "integer-multiply-overflows",
    "storage-bounds-check",
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
    "erase-reference",
    "erase-reference-mut",
    "restore-reference",
    "restore-reference-mut",
    "function-pointer-from-item",
    "function-pointer-invoke",
    "string-view-size",
    "string-view-empty",
    "array-range-begin",
    "array-size",
    "thread-handle-empty",
    "thread-spawn",
    "thread-joinable",
    "thread-join",
    "thread-result-take",
    "thread-auto-join",
    "atomic-load",
    "atomic-store",
    "atomic-exchange",
    "atomic-compare-exchange",
    "atomic-wait",
    "atomic-notify-one",
    "atomic-notify-all",
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
    "fail-empty-owner",
    "fail-inactive-value",
    "fail-inactive-error",
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
    "source-expression-text",
    "meta-field-count",
    "meta-field-name",
    "meta-field-is-public",
    "meta-field-has-default",
    "meta-type-name",
    "meta-type-kind",
    "meta-field-attribute-count",
    "meta-field-has-attribute",
    "meta-type-attribute-count",
    "meta-type-has-attribute",
    "meta-type-trait",
    "meta-type-relation",
    "meta-enumerator-count",
    "meta-enumerator-name",
    "meta-enumerator-value",
    "meta-payload-count",
    "meta-payload-name",
    "meta-base-count",
    "meta-argument-count",
    "meta-method-count",
    "meta-method-name",
    "meta-method-trait",
    "meta-method-parameter-count",
    "meta-parameter-name",
    "meta-constructor-count",
    "meta-constructor-parameter-count",
    "meta-constructor-parameter-name",
    "meta-declaration-line",
    "meta-declaration-file",
    "meta-type-primary-name",
    "meta-field-attribute",
    "meta-type-attribute",
    "meta-field-default",
    "meta-field-get",
    "meta-field-get-mut",
    "meta-construct",
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
    "object-relocate-range",
    "require-vacant-storage",
    "require-live-storage",
    "require-relocation-capacity",
    "require-relocation-source",
    "require-relocation-destination",
    "text-view-from-address",
    "c-string-from-address",
    "require-allocation",
    "thread-scope",
});

constexpr auto synchronizationNames = std::to_array<std::string_view>({
    "none",
    "thread-spawn",
    "thread-join",
    "atomic-load",
    "atomic-store",
    "atomic-read-modify-write",
    "atomic-compare-exchange",
    "atomic-wait",
    "atomic-notify",
    "mutex-lock",
    "mutex-unlock",
    "condition-wait",
    "condition-notify",
    "thread-scope",
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

bool isMirScalarValueType(const MirProgram *program, const SemanticType &type) {
  return constantIntegerDomain(type) || type.kind == SemanticType::Float ||
         type.kind == SemanticType::Double || type.kind == SemanticType::Bool ||
         type.kind == SemanticType::Char ||
         type.kind == SemanticType::NullPtr ||
         type.kind == SemanticType::RawPointer ||
         (program && type.kind == SemanticType::Enum &&
          program->findIntegralEnum(type));
}

namespace {
// The sealed zero/one-argument construction shape with no selected
// constructor, cleanup, adapter or lifecycle event. Returns its pointee type.
const SemanticType *
scalarFinalAddressPointee(const MirInstruction &instruction) {
  if (instruction.tombstone || instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectConstruct ||
      instruction.constructorTarget || instruction.elementCleanup ||
      instruction.receiver || instruction.nativeCallbackAdapter ||
      instruction.threadTaskAdapter || instruction.result ||
      instruction.info.type != SemanticType::Void ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.rawMemoryAccess || !instruction.lifecycle.empty() ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.constructorKind != ConstructorKind::Ordinary ||
      instruction.operation != MirOperation::None ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.operands.empty() || instruction.operands.size() > 2)
    return nullptr;
  const auto &address = instruction.operands.front().type;
  if (address.kind != SemanticType::RawPointer ||
      address.arguments.size() != 1 ||
      address.pointerAccess != AccessMode::Mutable ||
      (instruction.operands.size() == 2 &&
       instruction.operands.back().type != address.arguments.front()))
    return nullptr;
  return &address.arguments.front();
}
} // namespace

bool isMirScalarFinalAddressConstruction(const MirInstruction &instruction) {
  const auto *pointee = scalarFinalAddressPointee(instruction);
  return pointee && isMirScalarValueType(nullptr, *pointee);
}

namespace {
enum class StructuralValueOperation { Copy, Move, Drop };

bool structuralScalarRecord(const MirProgram &program, const SemanticType &type,
                            StructuralValueOperation operation) {
  std::unordered_set<HirClassInstanceId> visiting;
  std::size_t remaining = 1024;
  const auto structural = [&](const auto &self,
                              const SemanticType &type) -> bool {
    if (remaining == 0)
      return false;
    --remaining;
    if (isMirScalarValueType(&program, type))
      return true;
    // A closed payload enum is passive when every alternative's payload is:
    // its compiler-defined transfer and drop then act only on that payload.
    // Borrowed or actively cleaned payloads keep their own effects.
    if (const MirPayloadEnum *sum = type.kind == SemanticType::Enum
                                        ? program.findPayloadEnum(type)
                                        : nullptr) {
      const bool available =
          operation == StructuralValueOperation::Copy   ? sum->traits.copyable
          : operation == StructuralValueOperation::Move ? sum->traits.movable
                                                        : true;
      return available && !sum->traits.containsBorrowedState &&
             !sum->requiresActiveCleanup &&
             std::all_of(sum->variants.begin(), sum->variants.end(),
                         [&](const MirPayloadEnumVariant &variant) {
                           return std::all_of(variant.payloadTypes.begin(),
                                              variant.payloadTypes.end(),
                                              [&](const SemanticType &payload) {
                                                return self(self, payload);
                                              });
                         });
    }
    if (type.kind != SemanticType::Class)
      return false;
    const MirClassInstance *owner = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == type) {
        if (owner)
          return false;
        owner = &candidate;
      }
    if (!owner || owner->traits.containsBorrowedState ||
        owner->compilerCapability != CompilerCapabilityTypeKind::None ||
        owner->unionLayout || owner->abstract || owner->polymorphic ||
        !owner->bases.empty() || !owner->structuralBases.empty() ||
        !visiting.insert(owner->id).second)
      return false;
    const SpecialMemberStatus member =
        operation == StructuralValueOperation::Copy   ? owner->copyConstructor
        : operation == StructuralValueOperation::Move ? owner->moveConstructor
                                                      : owner->destructorStatus;
    const bool fields =
        member == SpecialMemberStatus::Generated &&
        (operation != StructuralValueOperation::Drop || !owner->destructor) &&
        std::all_of(owner->declaredFields.begin(), owner->declaredFields.end(),
                    [&](const auto &field) {
                      return field.field && field.symbol &&
                             self(self, field.type);
                    });
    visiting.erase(owner->id);
    return fields;
  };
  return type.kind == SemanticType::Class && structural(structural, type);
}
} // namespace

bool isMirStructuralFinalAddressConstruction(
    const MirProgram &program, const MirInstruction &instruction) {
  if (const auto *pointee = scalarFinalAddressPointee(instruction);
      pointee && isMirScalarValueType(&program, *pointee))
    return true;
  if (instruction.constructorTarget || instruction.operands.size() != 2 ||
      (instruction.constructorKind != ConstructorKind::Copy &&
       instruction.constructorKind != ConstructorKind::Move) ||
      !mir_detail::objectConstruct(program, instruction) ||
      std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                  [](const MirLifecycleEvent &event) {
                    return event.kind != MirLifecycleEventKind::TransferOut;
                  }))
    return false;
  return structuralScalarRecord(program, instruction.operands.back().type,
                                instruction.constructorKind ==
                                        ConstructorKind::Copy
                                    ? StructuralValueOperation::Copy
                                    : StructuralValueOperation::Move);
}

bool isMirStructuralValueTransfer(const MirProgram &program,
                                  const MirInstruction &instruction) {
  const bool copy = instruction.kind == MirInstructionKind::CallInput &&
                    instruction.callInputKind == HirCallInputKind::CopyValue;
  const bool move = instruction.kind == MirInstructionKind::Move ||
                    (instruction.kind == MirInstructionKind::CallInput &&
                     instruction.callInputKind == HirCallInputKind::MoveValue);
  if ((!copy && !move) || instruction.tombstone || !instruction.result ||
      instruction.operands.size() != 1 || instruction.receiver ||
      instruction.operation != MirOperation::None ||
      (instruction.intrinsic != IntrinsicKind::None &&
       !(instruction.kind == MirInstructionKind::Move &&
         instruction.intrinsic == IntrinsicKind::Move)) ||
      instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.hostedStartupOperation ||
      instruction.info.type != instruction.operands.front().type ||
      instruction.info.type.kind != SemanticType::Class ||
      instruction.info.traits.containsBorrowedState ||
      std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                  [](const MirLifecycleEvent &event) {
                    return event.kind != MirLifecycleEventKind::Initialize &&
                           event.kind != MirLifecycleEventKind::Move &&
                           event.kind != MirLifecycleEventKind::Reparent &&
                           event.kind != MirLifecycleEventKind::TransferOut;
                  }) ||
      (instruction.kind == MirInstructionKind::Move &&
       (instruction.destination ||
        instruction.operands.front().kind != MirOperandKind::Move)))
    return false;
  return structuralScalarRecord(program, instruction.info.type,
                                copy ? StructuralValueOperation::Copy
                                     : StructuralValueOperation::Move);
}

bool isMirStructuralValueDrop(const MirProgram &program,
                              const MirInstruction &instruction) {
  return !instruction.tombstone &&
         instruction.kind == MirInstructionKind::Drop &&
         instruction.destination && !instruction.result &&
         instruction.operands.empty() && !instruction.receiver &&
         !instruction.rawMemoryAccess && !instruction.hostedStartupOperation &&
         instruction.operation == MirOperation::None &&
         instruction.intrinsic == IntrinsicKind::None &&
         instruction.unsafeOperation == UnsafeOperationKind::None &&
         instruction.synchronization.kind ==
             SynchronizationOperationKind::None &&
         std::all_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                     [](const MirLifecycleEvent &event) {
                       return event.kind == MirLifecycleEventKind::Drop;
                     }) &&
         structuralScalarRecord(program, instruction.info.type,
                                StructuralValueOperation::Drop);
}

namespace {

[[nodiscard]] MirEffectTraits finishInstructionEffects(
    const MirInstruction &instruction, MirEffectTraits result,
    bool structuralValueTransfer = false, bool structuralConstruction = false,
    bool exactConstructor = false, bool passiveNativeCallback = false) {
  if (instruction.tombstone) {
    return harmless();
  }
  if (instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic != IntrinsicKind::None) {
    // With its exact source constructor merged separately, final-address
    // construction itself only initializes the vacant pointee and runs that
    // constructor; synchronization comes from the constructor's own summary.
    result = (structuralConstruction ||
              isMirScalarFinalAddressConstruction(instruction))
                 ? MirEffectTraits{.writesUnknownMemory = true,
                                   .initializesValue = true}
             : exactConstructor ? MirEffectTraits{.writesUnknownMemory = true,
                                                  .mayTrap = true,
                                                  .initializesValue = true,
                                                  .invokesUserCode = true}
                                : effects(instruction.intrinsic);
  } else if (instruction.kind == MirInstructionKind::Call &&
             instruction.synchronization.kind !=
                 SynchronizationOperationKind::None) {
    result = effects(instruction.synchronization.kind);
  } else if (result == MirEffectTraits{}) {
    result = effects(instruction.kind);
  }
  result = merge(result, (passiveNativeCallback ||
                          mir_detail::rawPointerConversion(instruction))
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
  if (instruction.kind == MirInstructionKind::Compute &&
      instruction.operation == MirOperation::Closure && instruction.loan)
    result.startsLoan = true;
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
    if (!structuralValueTransfer)
      result = merge(result, unknownEffects());
    result.copiesValue |=
        instruction.callInputKind == HirCallInputKind::CopyValue;
    result.movesValue |=
        instruction.callInputKind == HirCallInputKind::MoveValue;
    result.initializesValue = true;
    result.maySynchronize |= !structuralValueTransfer;
  }
  if (!instruction.lifecycle.empty()) {
    result = merge(result, effects(MirInstructionKind::Lifecycle));
    if (std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                    [&](const MirLifecycleEvent &event) {
                      return event.kind == MirLifecycleEventKind::Replace ||
                             (instruction.kind == MirInstructionKind::Assign &&
                              event.kind == MirLifecycleEventKind::TransferOut);
                    })) {
      // Replacement destroys the prior active value before publishing its
      // replacement, also when it converts the source into a distinct owning
      // wrapper. Until cleanup summaries exist, retain the same conservative
      // user-code/runtime ordering barrier as an explicit drop.
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
  case MirInstructionKind::EnsureThreadLocal:
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
  return finishInstructionEffects(
      instruction, result, false, false,
      instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::ObjectConstruct &&
          instruction.constructorTarget.has_value());
}

// The declared contract of GTI's own process-allocator runtime ABI, bound by
// the verified trusted declaration identity rather than a public spelling.
// Process allocation is not an observable language effect, and no operation
// synchronizes, runs GTI code, traps or retains an argument:
// - target queries and the alignment query read no memory; the latter only
//   inspects its address value;
// - allocation returns fresh storage or null; the POSIX form also writes its
//   `int32_t* status` pointee, stated as `writesUnknownMemory` because the
//   current vocabulary has no exact-pointee write;
// - release retires a block that, by the lease contract, holds no live object.
//   It is stated as `writesUnknownMemory` so no fact about the retired bytes
//   survives the call; the allocator's own internal ordering (C11's
//   free-to-next-allocation edge) carries no GTI-visible value.
// The result stays non-removable, non-reorderable and non-speculatable;
// allocation elision is a separate mechanism.
[[nodiscard]] std::optional<MirEffectSummary>
processMemoryRuntimeSummary(const MirFunctionInstance &function) {
  MirEffectTraits traits{.invokesRuntime = true, .targetDependent = true};
  switch (function.processMemoryOperation) {
  case ProcessMemoryRuntimeOperation::NativeSizeMax:
  case ProcessMemoryRuntimeOperation::AddressIsAligned:
  case ProcessMemoryRuntimeOperation::PosixPointerSize:
  case ProcessMemoryRuntimeOperation::PosixOutOfMemoryStatus:
  case ProcessMemoryRuntimeOperation::PosixInvalidArgumentStatus:
    break;
  case ProcessMemoryRuntimeOperation::PosixAllocate:
    traits.writesUnknownMemory = true;
    traits.allocates = true;
    break;
  case ProcessMemoryRuntimeOperation::AlignedAllocate:
    traits.allocates = true;
    break;
  case ProcessMemoryRuntimeOperation::PosixRelease:
  case ProcessMemoryRuntimeOperation::AlignedRelease:
    traits.writesUnknownMemory = true;
    break;
  case ProcessMemoryRuntimeOperation::None:
  case ProcessMemoryRuntimeOperation::Count:
    return std::nullopt;
  }
  MirEffectSummary result;
  result.traits = traits;
  result.argumentsEscape.assign(function.parameterTypes.size(), false);
  return result;
}

[[nodiscard]] const MirEffectSummary *
summaryForAddress(const MirProgram &program, MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        program.findFunctionInstance(address.owner);
    // A classified runtime declaration's stored summary is exact-checked
    // against its contract; other declarations remain unknown boundaries.
    return instance != nullptr &&
                   (instance->definitionKind == MirDefinitionKind::Source ||
                    processMemoryRuntimeSummary(*instance).has_value())
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
  case MirBodyKind::ThreadLocalInitializer: {
    const auto *storage = program.findThreadLocalStorage(address.owner);
    return storage == nullptr ? nullptr : &storage->effectSummary;
  }
  }
  return nullptr;
}

struct ExactTargets {
  bool boundary = false;
  bool nativeCleanup = false;
  bool synchronizingCleanup = false;
  std::vector<MirBodyAddress> addresses;
};

// A physical complete class value has exactly its declared dynamic type.
// Do not substitute a loan's canonical referent for a reference, projection,
// capture or constructor-phase receiver: those require a separate proof.
[[nodiscard]] bool exactVirtualBindingType(const MirProgram &program,
                                           const MirBody &body,
                                           const MirPlace &place) {
  // A second view of a reference binding cannot become a physical class
  // object merely by removing its Dereference projection.
  for (const auto &view : body.places)
    if (view.root == MirPlaceRootKind::Binding &&
        view.binding == place.binding && view.projections.empty() &&
        (view.capture != 0 || view.type != place.type))
      return false;
  const std::vector<HirBindingId> *bindings = nullptr;
  const std::vector<SemanticType> *types = nullptr;
  const auto formals = [&](const auto &instance) {
    if (&instance.body != &body)
      return false;
    bindings = &instance.parameterBindings;
    types = &instance.parameterTypes;
    return true;
  };
  if (body.kind == MirBodyKind::Function) {
    for (const auto &instance : program.functionInstances())
      if (formals(instance))
        break;
  } else if (body.kind == MirBodyKind::Constructor) {
    for (const auto &instance : program.constructorInstances())
      if (formals(instance))
        break;
  } else if (body.kind == MirBodyKind::Lambda) {
    for (const auto &instance : program.lambdaInstances())
      if (formals(instance))
        break;
  }
  if (bindings) {
    if (bindings->size() != types->size())
      return false;
    std::size_t matches = 0;
    for (std::size_t index = 0; index < bindings->size(); ++index) {
      if ((*bindings)[index] != place.binding)
        continue;
      if (++matches != 1 || (*types)[index] != place.type)
        return false;
    }
    if (matches == 1)
      return true;
  }
  // A nonformal local is a complete value only through an ordinary typed
  // Initialize publication, not a reference alias. Existing body lifetime
  // flow independently proves publication dominates this use and stays live.
  if (place.initiallyAvailable)
    return false;
  const auto valuePublication = [&](const MirOperand &operand) {
    if (operand.kind == MirOperandKind::Copy ||
        operand.kind == MirOperandKind::Move) {
      const auto *source = body.findPlace(operand.place);
      return source && source->type == place.type;
    }
    if (operand.kind != MirOperandKind::Value)
      return false;
    MirValueId current = operand.value;
    std::unordered_set<MirValueId> visited;
    while (visited.insert(current).second) {
      const auto *value = body.findValue(current);
      const auto *block =
          value ? body.findBlock(value->definitionBlock) : nullptr;
      if (!value || !block || value->info.type != place.type ||
          value->info.category != ValueCategory::Value)
        return false;
      const MirInstruction *producer = nullptr;
      for (const auto &instruction : block->instructions)
        if (instruction.id == value->definition)
          producer = &instruction;
      if (!producer || producer->tombstone || producer->result != current ||
          producer->info != value->info ||
          producer->kind == MirInstructionKind::CallInput)
        return false;
      if (producer->kind != MirInstructionKind::Compute ||
          producer->operation != MirOperation::Identity)
        return true;
      if (producer->operands.size() != 1 ||
          producer->operands.front().kind != MirOperandKind::Value ||
          producer->operands.front().type != place.type)
        return false;
      current = producer->operands.front().value;
    }
    return false;
  };
  bool initialized = false;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone ||
          instruction.kind != MirInstructionKind::Initialize ||
          !instruction.destination)
        continue;
      const auto *destination = body.findPlace(*instruction.destination);
      if (!destination || destination->root != MirPlaceRootKind::Binding ||
          destination->binding != place.binding ||
          !destination->projections.empty())
        continue;
      if (destination->type != place.type || instruction.operands.size() != 1 ||
          instruction.operands.front().type != place.type ||
          !valuePublication(instruction.operands.front()))
        return false;
      initialized = true;
    }
  return initialized;
}

[[nodiscard]] const MirClassInstance *
exactVirtualReceiver(const MirProgram &program, const MirBody &body,
                     const MirInstruction &instruction) {
  if (!instruction.receiver ||
      instruction.receiver->kind != MirOperandKind::Value ||
      instruction.callSite == 0)
    return nullptr;
  const auto *value = body.findValue(instruction.receiver->value);
  const auto *block = value ? body.findBlock(value->definitionBlock) : nullptr;
  if (!block)
    return nullptr;
  const auto found = std::find_if(
      block->instructions.begin(), block->instructions.end(),
      [&](const auto &candidate) { return candidate.id == value->definition; });
  if (found == block->instructions.end())
    return nullptr;
  const auto &input = *found;
  const bool shared = input.callInputKind == HirCallInputKind::ReadBorrow;
  const bool exclusive = input.callInputKind == HirCallInputKind::MutableBorrow;
  if (input.tombstone || input.kind != MirInstructionKind::CallInput ||
      input.callInputRole != MirCallInputRole::Receiver ||
      input.callSite != instruction.callSite ||
      input.result != instruction.receiver->value ||
      input.info.type != instruction.receiver->type ||
      value->info.type != input.info.type || (!shared && !exclusive) ||
      input.operands.size() != 1 ||
      input.operands.front().kind !=
          (shared ? MirOperandKind::BorrowRead : MirOperandKind::BorrowWrite))
    return nullptr;
  const auto *function =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  if (!function ||
      function->receiverMutability !=
          (shared ? ReceiverMutability::ReadOnly : ReceiverMutability::Mutable))
    return nullptr;
  const auto *place = body.findPlace(input.operands.front().place);
  if (!place || place->capture != 0 || place->foreignStorage ||
      !place->projections.empty() || place->type.kind != SemanticType::Class ||
      place->type != input.info.type ||
      place->type != input.operands.front().type ||
      (place->root != MirPlaceRootKind::Binding &&
       place->root != MirPlaceRootKind::Symbol))
    return nullptr;
  const auto closed = [&](const auto &self, const SemanticType &type) -> bool {
    if (type.kind == SemanticType::Unknown ||
        type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::TypePack ||
        type.kind == SemanticType::TypeName || type.arrayLengthParameterId != 0)
      return false;
    const auto closedValues = [](const auto &values) {
      return std::all_of(values.begin(), values.end(), [](const auto &value) {
        return value.kind == CompileTimeValue::UInt64;
      });
    };
    const auto closedTypes = [&](const auto &types) {
      return std::all_of(types.begin(), types.end(), [&](const auto &argument) {
        return self(self, argument);
      });
    };
    return closedValues(type.valueArguments) &&
           closedValues(type.lambdaEnclosingClassValues) &&
           closedValues(type.lambdaEnclosingFunctionValues) &&
           closedTypes(type.arguments) &&
           closedTypes(type.lambdaEnclosingClassTypes) &&
           closedTypes(type.lambdaEnclosingFunctionTypes);
  };
  if (!closed(closed, place->type))
    return nullptr;
  if (place->root == MirPlaceRootKind::Binding) {
    if (place->binding == 0 || !exactVirtualBindingType(program, body, *place))
      return nullptr;
  } else {
    const SemanticType *storedType = nullptr;
    if (const auto *storage = program.findThreadLocalStorage(place->symbol))
      storedType = &storage->type;
    else if (const auto *step =
                 program.programInitializationPlan().findStepForSymbol(
                     place->symbol)) {
      const auto *storage = program.module().findPlace(step->storagePlace);
      if (storage &&
          (storage->root == MirPlaceRootKind::Symbol ||
           (storage->root == MirPlaceRootKind::Binding &&
            storage->binding == step->binding)) &&
          storage->symbol == place->symbol && storage->capture == 0 &&
          storage->projections.empty() && !storage->foreignStorage)
        storedType = &storage->type;
    }
    if (!storedType || *storedType != place->type)
      return nullptr;
  }
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances()) {
    if (candidate.type != place->type)
      continue;
    if (owner)
      return nullptr;
    owner = &candidate;
  }
  return owner && !owner->abstract && !owner->cOpaqueHandle &&
                 owner->compilerCapability ==
                     CompilerCapabilityTypeKind::None &&
                 (owner->kind == ClassKind::Class ||
                  owner->kind == ClassKind::Struct)
             ? owner
             : nullptr;
}

// This selects an effect edge only. Execution remains a virtual call with its
// original Invoke/status ABI, staged loans and owning-result publication.
// Readonly is a receiver capability, never a proof of absent global effects.
[[nodiscard]] const MirFunctionInstance *
exactWholeObjectVirtualTarget(const MirProgram &program, const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.dispatch != CallDispatch::Virtual ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.nativeCallbackAdapter || instruction.nativeCalleeType ||
      !instruction.functionTarget)
    return nullptr;
  const auto *concrete = exactVirtualReceiver(program, body, instruction);
  const auto *selected =
      program.findFunctionInstance(*instruction.functionTarget);
  const auto *selectedOwner = selected && selected->owner
                                  ? program.findClassInstance(*selected->owner)
                                  : nullptr;
  if (!concrete || !selected || !selectedOwner || !selected->virtualMethod ||
      selected->staticMember || selected->linkage != LanguageLinkage::Gti ||
      selectedOwner->type != instruction.dispatchOwner ||
      selected->virtualRoots.empty())
    return nullptr;

  std::unordered_set<HirClassInstanceId> complete;
  std::unordered_set<HirClassInstanceId> active;
  const auto collect = [&](const auto &self, HirClassInstanceId id) -> bool {
    if (complete.contains(id))
      return true;
    const auto *owner = program.findClassInstance(id);
    if (!owner || !active.insert(id).second)
      return false;
    for (const auto &base : owner->bases) {
      const auto *baseOwner = program.findClassInstance(base.instance);
      if (!baseOwner || baseOwner->type != base.type ||
          !self(self, base.instance))
        return false;
    }
    active.erase(id);
    complete.insert(id);
    return true;
  };
  if (!collect(collect, concrete->id) || !complete.contains(selectedOwner->id))
    return nullptr;
  const auto derives = [&](HirClassInstanceId derived,
                           HirClassInstanceId base) {
    std::vector<HirClassInstanceId> pending{derived};
    std::unordered_set<HirClassInstanceId> seen;
    while (!pending.empty()) {
      const auto next = pending.back();
      pending.pop_back();
      if (next == base)
        return true;
      if (!seen.insert(next).second)
        continue;
      const auto *owner = program.findClassInstance(next);
      if (!owner)
        return false;
      for (const auto &parent : owner->bases)
        pending.push_back(parent.instance);
    }
    return false;
  };
  const auto sameContract = [&](const MirFunctionInstance &candidate) {
    return candidate.parameterTypes == selected->parameterTypes &&
           candidate.returnType == selected->returnType &&
           candidate.receiverMutability == selected->receiverMutability &&
           candidate.overloadedOperator == selected->overloadedOperator &&
           candidate.returnBorrowOrigin == selected->returnBorrowOrigin &&
           candidate.returnBorrowParameter == selected->returnBorrowParameter &&
           candidate.returnBorrowAccess == selected->returnBorrowAccess;
  };
  const auto containsRoot = [](const MirFunctionInstance &function,
                               FunctionId root) {
    return std::find(function.virtualRoots.begin(), function.virtualRoots.end(),
                     root) != function.virtualRoots.end();
  };

  const MirFunctionInstance *target = nullptr;
  std::unordered_set<FunctionId> roots;
  for (const auto rootId : selected->virtualRoots) {
    if (rootId == 0 || !roots.insert(rootId).second)
      return nullptr;
    const MirFunctionInstance *root = nullptr;
    for (const auto &candidate : program.functionInstances()) {
      if (candidate.declaration != rootId || !candidate.owner ||
          !complete.contains(*candidate.owner) || !sameContract(candidate))
        continue;
      if (root || !candidate.virtualMethod || candidate.staticMember ||
          candidate.overrideMethod ||
          candidate.linkage != LanguageLinkage::Gti ||
          !containsRoot(candidate, candidate.declaration) ||
          !derives(selectedOwner->id, *candidate.owner))
        return nullptr;
      root = &candidate;
    }
    if (!root)
      return nullptr;

    std::vector<const MirFunctionInstance *> members;
    for (const auto &candidate : program.functionInstances()) {
      if (!containsRoot(candidate, rootId))
        continue;
      if (!candidate.owner || !program.findClassInstance(*candidate.owner))
        return nullptr;
      if (!complete.contains(*candidate.owner))
        continue;
      if (!candidate.virtualMethod || candidate.staticMember ||
          candidate.linkage != LanguageLinkage::Gti ||
          !sameContract(candidate) ||
          !derives(*candidate.owner, *root->owner) ||
          (!candidate.overrideMethod && candidate.declaration != rootId))
        return nullptr;
      members.push_back(&candidate);
    }
    const MirFunctionInstance *final = nullptr;
    for (const auto *candidate : members) {
      if (!std::all_of(members.begin(), members.end(), [&](const auto *other) {
            return derives(*candidate->owner, *other->owner);
          }))
        continue;
      if (final)
        return nullptr;
      final = candidate;
    }
    // Keep the declaration and concrete instantiation selected by ordinary
    // member lookup. This proof does not remap a different override's borrow,
    // callable, field-publication or result contract onto the original call.
    if (!final || final->id != selected->id || final->pureVirtual ||
        final->definitionKind != MirDefinitionKind::Source ||
        (target && target != final))
      return nullptr;
    target = final;
  }
  return target;
}

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
             instruction.intrinsic == IntrinsicKind::ObjectConstruct &&
             instruction.constructorTarget) {
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
  } else if (instruction.kind == MirInstructionKind::Call &&
             instruction.dispatch != CallDispatch::Static) {
    if (const auto *target =
            exactWholeObjectVirtualTarget(program, body, instruction))
      result.addresses.push_back(
          {.kind = MirBodyKind::Function, .owner = target->id});
    else
      result.boundary = true;
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
    if (mir_detail::generatedDefaultConstruction(instruction)) {
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) {
            return candidate.type == instruction.info.type;
          });
      // Bodyless generated construction executes no source callable. A real
      // field/base schedule retains its separately selected constructor.
      if (owner != program.classInstances().end() &&
          owner->defaultConstructor == SpecialMemberStatus::Generated &&
          !owner->generatedDefaultRequiresSchedule)
        return result;
    }
    if (instruction.constructorTarget) {
      result.addresses.push_back(
          {.kind = MirBodyKind::Constructor,
           .owner = static_cast<std::size_t>(*instruction.constructorTarget)});
    } else {
      result.boundary = true;
    }
  } else if (instruction.kind == MirInstructionKind::CallBody ||
             instruction.kind == MirInstructionKind::EnsureThreadLocal) {
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
         instruction.kind == MirInstructionKind::CallBody ||
         instruction.kind == MirInstructionKind::EnsureThreadLocal;
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
    if (selected.kind == SemanticType::UniqueOwner &&
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
    if (candidate.kind == SemanticType::Lambda) {
      if (!candidate.hasLambdaShape())
        return RelocationKind::Unavailable;
      // A closure environment is a plain aggregate of its captures.
      auto result = RelocationKind::Bitwise;
      for (const auto &part : candidate.lambdaCaptureTypes())
        result = merge(result, self(self, part));
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
        owner->compilerCapability == CompilerCapabilityTypeKind::CString ||
        owner->compilerCapability ==
            CompilerCapabilityTypeKind::FunctionPointer) {
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
      const bool safe = owner->compilerCapability ==
                            CompilerCapabilityTypeKind::PolymorphicStorage ||
                        ((owner->compilerCapability ==
                              CompilerCapabilityTypeKind::OptionalStorage ||
                          owner->compilerCapability ==
                              CompilerCapabilityTypeKind::MutexStorage) &&
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

namespace {
// The body-aware query is only a lookup shortcut. Both public overloads must
// retain the same program-owned structural lifecycle classification.
std::optional<MirEffectTraits>
structuralInstructionEffects(const MirProgram &program,
                             const MirInstruction &instruction) {
  if (instruction.operation == MirOperation::NativeCallback &&
      instruction.nativeCallbackAdapter) {
    const auto *adapter =
        program.findNativeCallbackAdapter(*instruction.nativeCallbackAdapter);
    if (adapter &&
        adapter->cThreadCallback != CThreadCallbackKind::ThreadConfined)
      return finishInstructionEffects(instruction, {}, false, false, false,
                                      true);
  }
  if (isMirStructuralFinalAddressConstruction(program, instruction))
    return finishInstructionEffects(instruction, {}, false, true);
  if (isMirStructuralValueDrop(program, instruction))
    return finishInstructionEffects(instruction,
                                    {.writesPlace = true, .dropsValue = true});
  if (!isInvocation(instruction)) {
    return finishInstructionEffects(
        instruction, {}, isMirStructuralValueTransfer(program, instruction));
  }
  return std::nullopt;
}
} // namespace

MirEffectTraits effects(const MirProgram &program,
                        const MirInstruction &instruction) {
  if (const auto structural =
          structuralInstructionEffects(program, instruction))
    return *structural;
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
  if (const auto structural =
          structuralInstructionEffects(program, instruction))
    return *structural;
  const ExactTargets targets = exactTargets(program, body, instruction);
  if (targets.boundary)
    return cleanupEffects(merge(effects(instruction), unknownEffects()),
                          targets);
  if (targets.addresses.empty()) {
    if (mir_detail::generatedDefaultConstruction(instruction))
      return cleanupEffects(
          finishInstructionEffects(instruction, {.initializesValue = true}),
          targets);
    return cleanupEffects(effects(instruction), targets);
  }
  MirEffectTraits result =
      exactInvocationShell(instruction, targets.nativeCleanup);
  for (const MirBodyAddress target : targets.addresses) {
    result = merge(result, summaryForAddress(program, target)->traits);
  }
  return cleanupEffects(result, targets);
}

MirInvocationTargets mirInvocationTargets(const MirProgram &program,
                                          const MirBody &body,
                                          const MirInstruction &instruction) {
  if (!isInvocation(instruction))
    return {};
  ExactTargets targets = exactTargets(program, body, instruction);
  return {.boundary = targets.boundary,
          .addresses = std::move(targets.addresses)};
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

  for (const auto &storage : program.threadLocalStorage())
    nodes.push_back({.address = {.kind = MirBodyKind::ThreadLocalInitializer,
                                 .owner = storage.symbol},
                     .body = &storage.initializer,
                     .exactDefinition = true});

  for (EffectNode &node : nodes) {
    node.local.traits = harmless();
    node.local.argumentsEscape.assign(node.parameterCount, false);
    if (!node.exactDefinition || node.body == nullptr) {
      const MirFunctionInstance *declaration =
          node.address.kind == MirBodyKind::Function
              ? program.findFunctionInstance(node.address.owner)
              : nullptr;
      if (const auto declared = declaration == nullptr
                                    ? std::nullopt
                                    : processMemoryRuntimeSummary(*declaration);
          declared && !node.exactDefinition) {
        node.local = *declared;
        continue;
      }
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
          const auto structural =
              structuralInstructionEffects(program, instruction);
          const bool bodylessConstruction =
              !targets.boundary && targets.addresses.empty() &&
              mir_detail::generatedDefaultConstruction(instruction);
          const MirEffectTraits shell =
              structural ? *structural
              : bodylessConstruction
                  ? finishInstructionEffects(instruction,
                                             {.initializesValue = true})
                  : finishInstructionEffects(
                        instruction,
                        isMirStructuralValueDrop(program, instruction)
                            ? MirEffectTraits{.writesPlace = true,
                                              .dropsValue = true}
                            : MirEffectTraits{},
                        isMirStructuralValueTransfer(program, instruction),
                        isMirStructuralFinalAddressConstruction(program,
                                                                instruction));
          const MirEffectTraits instructionEffects = cleanupEffects(
              !structural && targets.boundary ? merge(shell, unknownEffects())
                                              : shell,
              targets);
          node.local.traits = merge(node.local.traits, instructionEffects);
          if (isInvocation(instruction) &&
              (targets.boundary ||
               (targets.addresses.empty() && !bodylessConstruction)) &&
              !isMirStructuralValueDrop(program, instruction) &&
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
      // The continuation's own endpoint records the trap.
      case MirTerminatorKind::ContinueFailure:
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
  for (std::size_t index = 0; index < program.threadLocalStorage().size();
       ++index)
    result.threadLocalInitializers.push_back(std::move(summaries[next++]));
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
      required.lambdas.size() != program.lambdaInstances().size() ||
      required.threadLocalInitializers.size() !=
          program.threadLocalStorage().size()) {
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
  for (std::size_t index = 0; index < required.threadLocalInitializers.size();
       ++index) {
    const auto &storage = program.threadLocalStorage()[index];
    if (!summaryCovers(storage.effectSummary,
                       required.threadLocalInitializers[index]))
      reject({.kind = MirBodyKind::ThreadLocalInitializer,
              .owner = storage.symbol},
             "thread-local initializer effect summary under-approximates its "
             "MIR body");
  }
  return result;
}

} // namespace lang
