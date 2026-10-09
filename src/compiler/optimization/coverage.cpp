#include "gti/optimization/coverage.h"

#include "gti/mir_effects.h"
#include "gti/optimization/pass.h"

#include <algorithm>
#include <optional>

// This translation unit is the exhaustive language-growth gate. In particular,
// a default label must not hide an added enumerator, including in enums without
// Count. Do not relax this diagnostic to add a language feature: classify its
// existing consumer or conservative barrier here first.
#if defined(__clang__)
#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wswitch-enum"
#elif defined(__GNUC__)
#pragma GCC diagnostic error "-Wswitch"
#pragma GCC diagnostic error "-Wswitch-enum"
#elif defined(_MSC_VER)
#pragma warning(error : 4061 4062)
#endif

namespace lang {
namespace {
constexpr MirOptimizationCoverage supported(std::string_view consumer) {
  return {MirOptimizationHandling::Supported, consumer};
}
constexpr MirOptimizationCoverage barrier(std::string_view consumer) {
  return {MirOptimizationHandling::Barrier, consumer};
}
constexpr MirOptimizationCoverage inapplicable(std::string_view consumer) {
  return {MirOptimizationHandling::NotApplicable, consumer};
}
} // namespace

MirOptimizationCoverage optimizationCoverage(MirInstructionKind kind) {
  switch (kind) {
  case MirInstructionKind::Compute:
    return supported("typed scalar folding/SCCP; effect-checked dead values");
  case MirInstructionKind::Load:
  case MirInstructionKind::Initialize:
  case MirInstructionKind::Assign:
    return supported("bounded scalar promotion and alias-aware range transfer");
  case MirInstructionKind::Modify:
    return supported("range transfer; alias-aware write invalidation");
  case MirInstructionKind::Call:
  case MirInstructionKind::Construct:
    return supported("exact call effects and bounded closed-call safety");
  case MirInstructionKind::Borrow:
  case MirInstructionKind::ActivateBorrow:
  case MirInstructionKind::EndBorrow:
    return barrier("loan/alias analysis; no loan schedule transformation");
  case MirInstructionKind::Move:
  case MirInstructionKind::CallInput:
  case MirInstructionKind::Lifecycle:
    return barrier("ownership/lifecycle effects; no transfer transformation");
  case MirInstructionKind::Drop:
  case MirInstructionKind::CleanupBeforeAssign:
    return barrier("exact cleanup effects; range invalidation");
  case MirInstructionKind::CallBody:
  case MirInstructionKind::InitializeBase:
    return barrier("exact body effects; no contextual range transfer");
  case MirInstructionKind::AllocateObject:
    return barrier("allocation, failure and rollback effects");
  case MirInstructionKind::StorageConstructPreflight:
    return barrier(
        "capacity preflight before prepared-parameter ownership transfer");
  case MirInstructionKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirOperation operation) {
  switch (operation) {
  case MirOperation::None:
    return inapplicable("instruction/intrinsic owns behavior");
  case MirOperation::Literal:
  case MirOperation::Identity:
  case MirOperation::Convert:
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Divide:
  case MirOperation::Remainder:
  case MirOperation::BitwiseAnd:
  case MirOperation::BitwiseOr:
  case MirOperation::BitwiseXor:
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight:
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
  case MirOperation::Positive:
  case MirOperation::Negate:
  case MirOperation::LogicalNot:
  case MirOperation::BitwiseNot:
    return supported(
        "typed folding/SCCP/ranges; unsupported types overdefined");
  case MirOperation::Index:
    return supported("alias-aware fixed-array bounds/range analysis");
  case MirOperation::Aggregate:
    return supported(
        "bounded array/object safety facts; effects govern removal");
  case MirOperation::Assign:
  case MirOperation::AddAssign:
  case MirOperation::SubtractAssign:
  case MirOperation::MultiplyAssign:
  case MirOperation::DivideAssign:
  case MirOperation::RemainderAssign:
  case MirOperation::BitwiseAndAssign:
  case MirOperation::BitwiseOrAssign:
  case MirOperation::BitwiseXorAssign:
  case MirOperation::ShiftLeftAssign:
  case MirOperation::ShiftRightAssign:
  case MirOperation::PreIncrement:
  case MirOperation::PreDecrement:
  case MirOperation::PostIncrement:
  case MirOperation::PostDecrement:
    return supported("range transfer or alias-aware write invalidation");
  case MirOperation::AddressOf:
  case MirOperation::PointerAdd:
  case MirOperation::PointerSubtract:
  case MirOperation::PointerDifference:
    return barrier("raw-memory/escape effects; no pointer range proof");
  case MirOperation::EnumConstant:
  case MirOperation::ExpectedHasValue:
  case MirOperation::Closure:
  case MirOperation::NativeCallback:
  case MirOperation::FunctionItem:
  case MirOperation::PayloadConstruct:
  case MirOperation::PayloadExtract:
  case MirOperation::Unexpected:
  case MirOperation::Comma:
    return supported("effects and use-def; scalar interpretation overdefined");
  case MirOperation::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::ObjectDestroy:
    return barrier("unsafe object lifetime end and typed cleanup");
  case IntrinsicKind::ObjectConstruct:
    return barrier("unsafe final-address construction and ownership handoff");
  case IntrinsicKind::ObjectRelocate:
    return barrier("unsafe destructive relocation and raw ownership transfer");
  case IntrinsicKind::ObjectSize:
  case IntrinsicKind::ObjectAlignment:
    return supported("pure target-dependent representation query; scalar "
                     "interpretation overdefined");
  case IntrinsicKind::RawBorrow:
  case IntrinsicKind::RawBorrowMut:
    return barrier("unsafe raw address to owner-witness checked loan");
  case IntrinsicKind::None:
    return inapplicable("ordinary instruction/call effects");
  case IntrinsicKind::NumericTypeParameterConversion:
  case IntrinsicKind::NumericAliasConversion:
  case IntrinsicKind::RawPointerConversion:
  case IntrinsicKind::EnumUnderlyingValue:
  case IntrinsicKind::StorageBoundsCheck:
  case IntrinsicKind::AllocatePrefixStorage:
  case IntrinsicKind::PrefixStorageAppend:
  case IntrinsicKind::PrefixStoragePop:
  case IntrinsicKind::PrefixStorageRead:
  case IntrinsicKind::PrefixStorageReadMut:
  case IntrinsicKind::PrefixStorageLength:
  case IntrinsicKind::PrefixStorageRelocate:
  case IntrinsicKind::PrefixStorageInsert:
  case IntrinsicKind::PrefixStorageErase:
    return supported(
        "effects and bounded safety transfer; exact outcome replay");
  case IntrinsicKind::DefaultTypeParameterConstruction:
  case IntrinsicKind::Move:
  case IntrinsicKind::AllocateUniqueOwner:
  case IntrinsicKind::UniqueOwnerBorrow:
  case IntrinsicKind::UniqueOwnerBorrowMut:
  case IntrinsicKind::UniqueOwnerIsNull:
  case IntrinsicKind::UniqueOwnerSwap:
  case IntrinsicKind::UniqueOwnerUpcast:
  case IntrinsicKind::IntegerWrappingAdd:
  case IntrinsicKind::IntegerWrappingSubtract:
  case IntrinsicKind::IntegerWrappingMultiply:
  case IntrinsicKind::IntegerSaturatingAdd:
  case IntrinsicKind::IntegerSaturatingSubtract:
  case IntrinsicKind::IntegerSaturatingMultiply:
  case IntrinsicKind::IntegerCheckedAdd:
  case IntrinsicKind::IntegerCheckedSubtract:
  case IntrinsicKind::IntegerCheckedMultiply:
  case IntrinsicKind::PrefixStorageTextView:
  case IntrinsicKind::PrefixStorageCString:
  case IntrinsicKind::PrefixStorageTryCString:
  case IntrinsicKind::OptionalStorageHasValue:
  case IntrinsicKind::OptionalStorageConstruct:
  case IntrinsicKind::OptionalStorageRead:
  case IntrinsicKind::OptionalStorageReadMut:
  case IntrinsicKind::OptionalStorageReset:
  case IntrinsicKind::PolymorphicStorageHasValue:
  case IntrinsicKind::PolymorphicStorageConstruct:
  case IntrinsicKind::PolymorphicStorageRead:
  case IntrinsicKind::PolymorphicStorageReadMut:
  case IntrinsicKind::PolymorphicStorageReset:
  case IntrinsicKind::StringViewSize:
  case IntrinsicKind::StringViewEmpty:
  case IntrinsicKind::ExpectedValue:
  case IntrinsicKind::ExpectedError:
  case IntrinsicKind::ExpectedValueOr:
  case IntrinsicKind::ArrayRangeBegin:
  case IntrinsicKind::ArraySize:
  case IntrinsicKind::ExpectedHasValue:
  case IntrinsicKind::ThreadHandleEmpty:
  case IntrinsicKind::ThreadJoinable:
  case IntrinsicKind::MutexGuardActive:
  case IntrinsicKind::MutexGuardRead:
  case IntrinsicKind::MutexGuardReadMut:
  case IntrinsicKind::ExactTypeParameterConstruction:
  case IntrinsicKind::SharedOwnerEmpty:
  case IntrinsicKind::SharedOwnerFromUnique:
  case IntrinsicKind::SharedOwnerClone:
  case IntrinsicKind::SharedOwnerBorrow:
  case IntrinsicKind::SharedOwnerIsNull:
  case IntrinsicKind::SharedOwnerUseCount:
  case IntrinsicKind::SharedOwnerSwap:
  case IntrinsicKind::SharedOwnerUpcast:
  case IntrinsicKind::SourceFileName:
  case IntrinsicKind::SourceFunctionName:
  case IntrinsicKind::SourceLine:
  case IntrinsicKind::SourceColumn:
  case IntrinsicKind::SimdAdd:
  case IntrinsicKind::SimdSubtract:
  case IntrinsicKind::SimdMultiply:
  case IntrinsicKind::SimdDivide:
  case IntrinsicKind::SimdEqual:
  case IntrinsicKind::SimdLess:
    return barrier(
        "authoritative intrinsic effects; no range/call interpretation");
  case IntrinsicKind::RestoringExchange:
  case IntrinsicKind::RestoringSwap:
  case IntrinsicKind::RestoringIndexedSwap:
    return barrier("exclusive restoring ownership transfer; mutable place "
                   "facts invalidated");
  case IntrinsicKind::ThreadSpawn:
  case IntrinsicKind::ThreadJoin:
  case IntrinsicKind::ThreadAutoJoin:
  case IntrinsicKind::AtomicLoad:
  case IntrinsicKind::AtomicStore:
  case IntrinsicKind::AtomicExchange:
  case IntrinsicKind::AtomicCompareExchange:
  case IntrinsicKind::MutexLock:
  case IntrinsicKind::MutexUnlock:
    return barrier("synchronization effects; forget mutable memory facts");
  case IntrinsicKind::MutexLockFailure:
  case IntrinsicKind::RequireKey:
  case IntrinsicKind::RequireContainerArgument:
  case IntrinsicKind::RequireVacantStorage:
  case IntrinsicKind::RequireLiveStorage:
  case IntrinsicKind::RequireRelocationCapacity:
  case IntrinsicKind::RequireRelocationSource:
  case IntrinsicKind::RequireRelocationDestination:
    return barrier("defined failure without a discharge proof family");
  case IntrinsicKind::Panic:
    return barrier("explicit terminal failure with bounded message capture");
  case IntrinsicKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirTerminatorKind kind) {
  switch (kind) {
  case MirTerminatorKind::Goto:
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Switch:
    return supported("CFG/dominance/loops and executable-edge dataflow");
  case MirTerminatorKind::Invoke:
    return supported("CFG and pre-invocation failure-state dataflow");
  case MirTerminatorKind::Return:
    return supported("CFG exit; return escape and bounded closed-call facts");
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return barrier("CFG exit with preserved failure/cleanup schedule");
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::Unreachable:
    return supported("CFG exit; no outgoing dataflow edges");
  case MirTerminatorKind::None:
    return inapplicable("no outgoing edges; structural verifier restricts use");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirProjectionKind kind) {
  switch (kind) {
  case MirProjectionKind::Field:
  case MirProjectionKind::Index:
  case MirProjectionKind::PackElement:
  case MirProjectionKind::Base:
    return supported(
        "structural alias relations; bounded projected range facts");
  case MirProjectionKind::Dereference:
    return barrier("indirect place may-alias unless exact loan facts suffice");
  case MirProjectionKind::RawIndex:
  case MirProjectionKind::RawDereference:
    return barrier(
        "raw memory may-alias; no disjointness from projection syntax");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirPlaceRootKind kind) {
  switch (kind) {
  case MirPlaceRootKind::Binding:
  case MirPlaceRootKind::Symbol:
  case MirPlaceRootKind::This:
  case MirPlaceRootKind::Temporary:
    return supported("exact root alias identity; bounded place facts");
  case MirPlaceRootKind::Value:
  case MirPlaceRootKind::Loan:
    return barrier("indirect root may-alias; exact provenance required");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Function:
  case MirBodyKind::Constructor:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
    return supported(
        "body inventory, analysis cache, editor and source replay");
  case MirBodyKind::HostedStartup:
    return barrier("body inventory/replay; generated startup effects retained");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirOperandKind kind) {
  switch (kind) {
  case MirOperandKind::Value:
  case MirOperandKind::Constant:
    return supported("use-def/edge arguments and typed scalar facts");
  case MirOperandKind::Copy:
    return supported("place effects; bounded scalar loads");
  case MirOperandKind::Address:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
  case MirOperandKind::Loan:
    return barrier("escape/ownership/loan effects; no scalar substitution");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirLifecycleEventKind kind) {
  switch (kind) {
  case MirLifecycleEventKind::Initialize:
  case MirLifecycleEventKind::Move:
  case MirLifecycleEventKind::Reparent:
  case MirLifecycleEventKind::Replace:
  case MirLifecycleEventKind::TransferOut:
  case MirLifecycleEventKind::Drop:
    return barrier("lifecycle verification/effects; preserve exact schedule");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(DefinedFailureCode code) {
  switch (code) {
  case DefinedFailureCode::None:
    return inapplicable("no defined-failure outcome");
  case DefinedFailureCode::IntegerOverflow:
  case DefinedFailureCode::ModuloByZero:
  case DefinedFailureCode::IndexOutOfBounds:
    return supported("bounded safety discharge; exact origin/outcome replay");
  case DefinedFailureCode::InvalidStorageState:
  case DefinedFailureCode::DivisionByZero:
  case DefinedFailureCode::NegativeShiftCount:
  case DefinedFailureCode::ShiftCountOutOfRange:
  case DefinedFailureCode::NumericConversionOutOfRange:
  case DefinedFailureCode::EmptyOwnerAccess:
  case DefinedFailureCode::InvalidExpectedAccess:
  case DefinedFailureCode::AllocationFailure:
  case DefinedFailureCode::InfallibleHostOperationFailed:
  case DefinedFailureCode::HostedRuntimeContractFailure:
  case DefinedFailureCode::KeyNotFound:
  case DefinedFailureCode::InvalidContainerArgument:
    return barrier(
        "unsupported failure discharge family; retain exact outcome");
  case DefinedFailureCode::Panic:
    return barrier("explicit terminal failure; preserve message and cleanup");
  case DefinedFailureCode::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(DefinedFailureDetail detail) {
  switch (detail) {
  case DefinedFailureDetail::None:
    return inapplicable("no defined-failure detail");
  case DefinedFailureDetail::Addition:
  case DefinedFailureDetail::Subtraction:
  case DefinedFailureDetail::Multiplication:
  case DefinedFailureDetail::Division:
  case DefinedFailureDetail::Negation:
  case DefinedFailureDetail::IntegerDivision:
  case DefinedFailureDetail::IntegerModulo:
  case DefinedFailureDetail::LeftShift:
  case DefinedFailureDetail::RightShift:
  case DefinedFailureDetail::NumericCast:
  case DefinedFailureDetail::HostedArgumentCount:
  case DefinedFailureDetail::FixedArray:
  case DefinedFailureDetail::StringView:
  case DefinedFailureDetail::Vector:
  case DefinedFailureDetail::String:
  case DefinedFailureDetail::PrivateStorage:
  case DefinedFailureDetail::Dereference:
  case DefinedFailureDetail::MemberAccess:
  case DefinedFailureDetail::ValueOnError:
  case DefinedFailureDetail::ErrorOnValue:
  case DefinedFailureDetail::DuplicateConstruction:
  case DefinedFailureDetail::UninitializedAccess:
  case DefinedFailureDetail::RelocationCapacity:
  case DefinedFailureDetail::InvalidRelocationSource:
  case DefinedFailureDetail::OccupiedRelocationDestination:
  case DefinedFailureDetail::UniqueOwner:
  case DefinedFailureDetail::ElementConstruction:
  case DefinedFailureDetail::HostedArguments:
  case DefinedFailureDetail::StdoutWrite:
  case DefinedFailureDetail::AutomaticJoin:
  case DefinedFailureDetail::NegativeArgumentCount:
  case DefinedFailureDetail::RecursiveThreadLocalInitialization:
  case DefinedFailureDetail::MutexLock:
  case DefinedFailureDetail::KeyLookup:
  case DefinedFailureDetail::ContainerArgument:
  case DefinedFailureDetail::SharedOwner:
    return supported(
        "exact failure identity; code/operation determine proof eligibility");
  case DefinedFailureDetail::ExplicitPanic:
    return barrier("explicit terminal failure; preserve message and cleanup");
  case DefinedFailureDetail::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(FailurePropagationKind kind) {
  switch (kind) {
  case FailurePropagationKind::None:
    return inapplicable("no propagated failure");
  case FailurePropagationKind::DirectCall:
  case FailurePropagationKind::Constructor:
    return supported("bounded closed-call facts; preserve invoke failure edge");
  case FailurePropagationKind::VirtualCall:
  case FailurePropagationKind::Callable:
  case FailurePropagationKind::TaskJoin:
  case FailurePropagationKind::BodyCall:
  case FailurePropagationKind::Destructor:
    return barrier("unsupported contextual failure/cleanup boundary");
  case FailurePropagationKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage
optimizationCoverage(SynchronizationOperationKind kind) {
  switch (kind) {
  case SynchronizationOperationKind::None:
    return inapplicable("no explicit synchronization");
  case SynchronizationOperationKind::ThreadSpawn:
  case SynchronizationOperationKind::ThreadJoin:
  case SynchronizationOperationKind::AtomicLoad:
  case SynchronizationOperationKind::AtomicStore:
  case SynchronizationOperationKind::AtomicReadModifyWrite:
  case SynchronizationOperationKind::AtomicCompareExchange:
  case SynchronizationOperationKind::MutexLock:
  case SynchronizationOperationKind::MutexUnlock:
  case SynchronizationOperationKind::ConditionWait:
  case SynchronizationOperationKind::ConditionNotify:
    return barrier(
        "synchronization effects; no motion or memory fact propagation");
  case SynchronizationOperationKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(AtomicMemoryOrder order) {
  switch (order) {
  case AtomicMemoryOrder::Relaxed:
  case AtomicMemoryOrder::Acquire:
  case AtomicMemoryOrder::Release:
  case AtomicMemoryOrder::AcquireRelease:
  case AtomicMemoryOrder::SequentiallyConsistent:
    return barrier("all atomic orders retain synchronization barrier");
  case AtomicMemoryOrder::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(UnsafeOperationKind kind) {
  switch (kind) {
  case UnsafeOperationKind::None:
    return inapplicable("no unsafe operation");
  case UnsafeOperationKind::RawDereference:
  case UnsafeOperationKind::RawIndex:
  case UnsafeOperationKind::RawMember:
  case UnsafeOperationKind::UnionMember:
  case UnsafeOperationKind::PointerArithmetic:
  case UnsafeOperationKind::ForeignPointerCall:
  case UnsafeOperationKind::DeclaredUnsafeCall:
  case UnsafeOperationKind::ObjectLifetime:
    return barrier("unsafe/raw-memory alias and escape boundary");
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirAnalysisKind kind) {
  switch (kind) {
  case MirAnalysisKind::CfgReachability:
  case MirAnalysisKind::Dominance:
  case MirAnalysisKind::ValueUses:
  case MirAnalysisKind::Effects:
  case MirAnalysisKind::ScalarPromotionInputs:
  case MirAnalysisKind::PlaceAliasEscape:
  case MirAnalysisKind::Loops:
  case MirAnalysisKind::SparseConstants:
    return supported("snapshot-scoped body cache; explicit edit invalidation");
  case MirAnalysisKind::SafetyChecks:
    return supported("body proof cache; local and program-wide invalidation");
  case MirAnalysisKind::ClosedCallSafety:
    return supported("program revision; dependent body proof invalidation");
  case MirAnalysisKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirTransformKind kind) {
  switch (kind) {
  case MirTransformKind::ReplaceInstruction:
  case MirTransformKind::RemoveInstruction:
  case MirTransformKind::ReplaceTerminator:
  case MirTransformKind::RemoveBlock:
  case MirTransformKind::PromotePlace:
    return supported(
        "controlled editor identity repair and canonical source replay");
  case MirTransformKind::DischargeSafetyCheck:
    return supported(
        "proof-only metadata; source replay before backend admission");
  case MirTransformKind::DevirtualizeCall:
    return supported("exact local receiver refinement and canonical replay");
  case MirTransformKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirTransformProofKind kind) {
  switch (kind) {
  case MirTransformProofKind::IdentityFold:
  case MirTransformProofKind::ComputeFold:
  case MirTransformProofKind::SparseConstant:
  case MirTransformProofKind::BranchFold:
  case MirTransformProofKind::UnreachableBlock:
  case MirTransformProofKind::DeadUnusedComputation:
  case MirTransformProofKind::ScalarPromotion:
  case MirTransformProofKind::BoundsRange:
  case MirTransformProofKind::ArithmeticRange:
  case MirTransformProofKind::ExactReceiver:
  case MirTransformProofKind::ExactParameterReceiver:
    return supported(
        "independent typed proof replay from canonical source MIR");
  case MirTransformProofKind::Count:
    break;
  }
  return {};
}

MirOptimizationCoverage optimizationCoverage(MirSafetyCheckFamily kind) {
  switch (kind) {
  case MirSafetyCheckFamily::Bounds:
  case MirSafetyCheckFamily::ArithmeticOverflow:
  case MirSafetyCheckFamily::ArithmeticDomain:
    return supported("safety proof/replay and per-family report census");
  case MirSafetyCheckFamily::Count:
    break;
  }
  return {};
}

MirVerificationResult verifyMirOptimizationVocabulary(const MirBody &body,
                                                      std::size_t owner) {
  MirVerificationResult result;
  MirBlockId blockId = 0;
  MirInstructionId instructionId = 0;
  const auto check = [&](auto value, std::string_view family) {
    if (!optimizationCoverage(value).classified()) {
      result.errors.push_back(
          {.bodyKind = body.kind,
           .owner = owner,
           .block = blockId,
           .instruction = instructionId,
           .message = "unclassified MIR optimizer " + std::string(family)});
    }
  };
  const auto operand = [&](const MirOperand &value) {
    check(value.kind, "operand kind");
  };
  const auto lifecycle = [&](const auto &events) {
    for (const MirLifecycleEvent &event : events) {
      check(event.kind, "lifecycle event");
    }
  };
  check(body.kind, "body kind");
  for (const MirPlace &place : body.places) {
    check(place.root, "place root");
    for (const MirPlaceProjection &projection : place.projections) {
      check(projection.kind, "place projection");
    }
  }
  for (const MirBlock &block : body.blocks) {
    blockId = block.id;
    instructionId = 0;
    const MirTerminator &terminator = block.terminator;
    check(terminator.kind, "terminator");
    if (terminator.value)
      operand(*terminator.value);
    for (const MirOperand &value : terminator.targetArguments)
      operand(value);
    for (const MirOperand &value : terminator.elseArguments)
      operand(value);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      for (const MirOperand &value : target.arguments)
        operand(value);
    }
    lifecycle(terminator.successLifecycle);
    for (const MirInstruction &instruction : block.instructions) {
      instructionId = instruction.id;
      check(instruction.kind, "instruction kind");
      check(instruction.operation, "operation");
      check(instruction.intrinsic, "intrinsic");
      check(instruction.unsafeOperation, "unsafe operation");
      check(instruction.synchronization.kind, "synchronization operation");
      if (instruction.synchronization.order)
        check(*instruction.synchronization.order, "atomic memory order");
      if (instruction.synchronization.failureOrder)
        check(*instruction.synchronization.failureOrder,
              "atomic failure order");
      if (instruction.bodyTarget)
        check(instruction.bodyTarget->kind, "body target");
      if (instruction.receiver)
        operand(*instruction.receiver);
      for (const MirOperand &value : instruction.operands)
        operand(value);
      lifecycle(instruction.lifecycle);
      check(instruction.definedFailure.propagation, "failure propagation");
      for (const MirSafetyDischarge &discharge : instruction.safetyDischarges) {
        check(discharge.family, "safety-check family");
        check(discharge.outcome.code, "discharged failure code");
        check(discharge.outcome.detail, "discharged failure detail");
      }
      for (const DefinedFailureOrigin &origin :
           instruction.definedFailure.localOrigins) {
        for (const DefinedFailureOutcome outcome : origin.outcomes) {
          check(outcome.code, "failure code");
          check(outcome.detail, "failure detail");
        }
      }
    }
  }
  return result;
}

std::string mirOptimizationBarrierReason(const MirProgram &program,
                                         const MirInstruction &instruction,
                                         const MirBody *body) {
  if (instruction.tombstone)
    return {};
  std::string result;
  const auto add = [&](std::string_view reason) {
    if (!result.empty())
      result += "; ";
    result += reason;
  };
  if (!optimizationCoverage(instruction.kind).classified() ||
      !optimizationCoverage(instruction.operation).classified() ||
      !optimizationCoverage(instruction.intrinsic).classified()) {
    add("unclassified MIR operation");
  }
  const MirEffectTraits effect = effects(program, instruction);
  if (instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None)
    add("unsafe/raw-memory access");
  if (instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic == IntrinsicKind::None && effect.invokesUserCode) {
    const auto summary =
        body == nullptr
            ? std::nullopt
            : exactMirInvocationSummary(program, *body, instruction);
    if (!summary) {
      add("call without a closed effect summary (native, virtual, recursive or "
          "unknown)");
    } else {
      add("user-code invocation with exact effect summary");
      if (summary->receiverEscapes ||
          std::any_of(summary->argumentsEscape.begin(),
                      summary->argumentsEscape.end(),
                      [](bool escapes) { return escapes; })) {
        add("receiver/argument escape");
      }
    }
  }
  if (effect.maySynchronize)
    add("possible synchronization");
  if (effect.readsUnknownMemory || effect.writesUnknownMemory)
    add("unknown memory/alias effects");
  if (effect.dropsValue || !instruction.lifecycle.empty() ||
      instruction.cleanupBoundaryEnd != 0 || instruction.fullExpressionEnd != 0)
    add("cleanup/lifecycle schedule");
  if (effect.startsLoan || effect.endsLoan || effect.dependsOnLoan ||
      instruction.loan)
    add("loan dependency");
  if (effect.movesValue || effect.initializesValue || instruction.ownership)
    add("ownership transfer or initialization");
  if (effect.allocates)
    add("allocation");
  if (effect.writesPlace)
    add("place mutation");
  if (effect.mayTrap)
    add("possible trap/defined failure");
  if (effect.targetDependent)
    add("target-dependent operation");
  if (effect.invokesRuntime)
    add("runtime invocation");
  for (const DefinedFailureOrigin &origin :
       instruction.definedFailure.localOrigins) {
    for (const DefinedFailureOutcome outcome : origin.outcomes) {
      if (optimizationCoverage(outcome.code).handling ==
          MirOptimizationHandling::Barrier) {
        add("unsupported failure discharge family");
        return result;
      }
    }
  }
  return result;
}

} // namespace lang
