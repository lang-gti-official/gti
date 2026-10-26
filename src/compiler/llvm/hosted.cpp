#include "hosted.h"
#include "abi.h"
#include "session.h"

#include <algorithm>
#include <limits>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/BuildLibCalls.h>
#include <stdexcept>

namespace lang::native_llvm {

std::optional<std::array<SemanticType, 2>>
hostedNativeParameters(const TargetInfo &target) {
  const auto integer = target.nativeCScalar(NativeCScalarKind::Int);
  const auto character =
      SemanticType::nativeCScalarType(NativeCScalarKind::Char, target);
  const auto size = target.nativeCScalar(NativeCScalarKind::Size);
  if (!target.supported() || !integer || !character || !size ||
      integer->valueKind != TargetScalarKind::Int32 ||
      integer->layout.sizeBytes != 4 ||
      (size->valueKind != TargetScalarKind::UInt32 &&
       size->valueKind != TargetScalarKind::UInt64) ||
      size->layout.sizeBytes == 0 || size->layout.sizeBytes > 8 ||
      size->layout.sizeBytes * 8 != target.dataLayout.pointerWidthBits())
    return std::nullopt;
  const auto count =
      SemanticType::nativeCScalarType(NativeCScalarKind::Int, target);
  if (!count)
    return std::nullopt;
  return std::array{*count, SemanticType::rawPointerTo(
                                SemanticType::rawPointerTo(*character))};
}

FunctionAbi hostedEntryAbi(Session &session) {
  const auto &plan = session.program().mir().hostedStartupPlan();
  const auto parameters = hostedNativeParameters(session.program().target());
  if (!plan || !parameters ||
      (plan->kind != ProgramEntryKind::NoArguments &&
       plan->kind != ProgramEntryKind::OwnedArguments))
    throw BackendUnsupportedError(
        "LLVM hosted entry needs a supported native C target and sealed plan");
  return session.abi().nativeC(parameters->front(),
                               plan->kind == ProgramEntryKind::OwnedArguments
                                   ? std::span<const SemanticType>(*parameters)
                                   : std::span<const SemanticType>{});
}

const MirHostedStartupOperation *
hostedArgumentOperation(const MirProgram &program, const MirBody &body,
                        const MirBlock &block,
                        const MirInstruction &instruction) {
  const auto &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::OwnedArguments ||
      program.hostedStartup() != &body || body.findBlock(block.id) != &block)
    return nullptr;
  const auto *operation =
      plan->findOperation(instruction.hostedStartupOperation);
  if (!operation ||
      (operation->kind !=
           MirHostedStartupOperationKind::ValidateArgumentCount &&
       operation->kind != MirHostedStartupOperationKind::ConvertArgumentCount &&
       operation->kind != MirHostedStartupOperationKind::ReadArgumentView))
    return nullptr;
  const auto member = std::find_if(
      block.instructions.begin(), block.instructions.end(),
      [&](const auto &candidate) { return &candidate == &instruction; });
  if (member == block.instructions.end() || operation->block != block.id ||
      operation->instruction != instruction.id || operation->terminator ||
      !instruction.result || operation->value != *instruction.result)
    throw std::logic_error("LLVM hosted operation lost its exact "
                           "body/instruction/result identity");
  const auto *value = body.findValue(*instruction.result);
  if (!value || value->hostedStartupOperation != operation->id ||
      value->definition != instruction.id || value->definitionBlock != block.id)
    throw std::logic_error("LLVM hosted result lost its producer identity");
  if (operation->kind == MirHostedStartupOperationKind::ValidateArgumentCount) {
    if (instruction.kind != MirInstructionKind::Load ||
        instruction.info.type != SemanticType::Int64 ||
        !instruction.operands.empty())
      throw std::logic_error("LLVM hosted count load lost its sealed shape");
  } else {
    const auto sourceKind =
        operation->kind == MirHostedStartupOperationKind::ConvertArgumentCount
            ? MirHostedStartupOperationKind::ValidateArgumentCount
            : MirHostedStartupOperationKind::LoadArgumentIndex;
    const auto source =
        std::find_if(plan->operations.begin(), plan->operations.end(),
                     [&](const auto &row) { return row.kind == sourceKind; });
    const auto inputType =
        sourceKind == MirHostedStartupOperationKind::ValidateArgumentCount
            ? SemanticType::Int64
            : SemanticType::Int32;
    const auto resultType =
        sourceKind == MirHostedStartupOperationKind::ValidateArgumentCount
            ? SemanticType::Int32
            : SemanticType::StringView;
    if (source == plan->operations.end() ||
        instruction.kind != MirInstructionKind::Compute ||
        instruction.operation != MirOperation::None ||
        instruction.info.type != resultType ||
        instruction.operands.size() != 1 ||
        instruction.operands[0].kind != MirOperandKind::Value ||
        instruction.operands[0].value != source->value ||
        instruction.operands[0].type != inputType)
      throw std::logic_error(
          "LLVM hosted operation lost its exact input identity");
  }
  return operation;
}

llvm::Value *hostedCountOutOfRange(llvm::IRBuilder<> &ir, llvm::Value *count) {
  if (!count || !count->getType()->isIntegerTy(64))
    throw std::logic_error("LLVM hosted count conversion requires Int64 input");
  return ir.CreateOr(
      ir.CreateICmpSLT(count,
                       ir.getInt64(std::numeric_limits<std::int32_t>::min())),
      ir.CreateICmpSGT(count,
                       ir.getInt64(std::numeric_limits<std::int32_t>::max())));
}

llvm::Value *hostedArgumentView(Session &session, llvm::IRBuilder<> &ir,
                                llvm::Value *arguments, llvm::Value *index) {
  if (!hostedNativeParameters(session.program().target()) || !arguments ||
      !arguments->getType()->isPointerTy() || !index ||
      !index->getType()->isIntegerTy(32))
    throw std::logic_error(
        "LLVM hosted view requires native argv and the sealed Int32 index");
  auto *slot = ir.CreateGEP(session.layouts().pointer(), arguments, index);
  auto *bytes = ir.CreateLoad(session.layouts().pointer(), slot);
  llvm::TargetLibraryInfoImpl implementation(
      llvm::Triple(session.module().getTargetTriple()));
  llvm::TargetLibraryInfo libraries(implementation);
  auto *length = llvm::emitStrLen(bytes, ir, session.dataLayout(), &libraries);
  const auto size = SemanticType::nativeCScalarType(NativeCScalarKind::Size,
                                                    session.program().target());
  if (!length || !size ||
      length->getType() != session.layouts().get(*size).value ||
      !length->getType()->isIntegerTy() ||
      length->getType()->getIntegerBitWidth() > 64)
    throw BackendUnsupportedError("LLVM hosted argv requires the exact target "
                                  "libc size_t strlen contract");
  if (!length->getType()->isIntegerTy(64))
    length = ir.CreateZExt(length, ir.getInt64Ty());
  auto *view = llvm::UndefValue::get(session.abi().textView());
  return ir.CreateInsertValue(ir.CreateInsertValue(view, bytes, 0), length, 1);
}

llvm::Value *hostedReturnStatus(llvm::IRBuilder<> &ir,
                                const MirHostedStartupOperation &operation,
                                llvm::Value *result) {
  if (operation.kind != MirHostedStartupOperationKind::ReturnEntry ||
      !operation.terminator ||
      operation.failureBehavior != MirHostedStartupFailureBehavior::None ||
      !result || !result->getType()->isIntegerTy(32))
    throw std::logic_error(
        "LLVM hosted return requires an exact Int32 ReturnEntry operation");
  // Unsigned comparison also rejects negative GTI results, without letting
  // host exit-status truncation turn a nonzero result into success.
  auto *portable = ir.CreateICmpULE(result, ir.getInt32(255));
  return ir.CreateSelect(portable, result, ir.getInt32(1));
}

std::optional<Literal> hostedSourceLiteral(const MirInstruction &instruction) {
  const bool text = instruction.intrinsic == IntrinsicKind::SourceFileName ||
                    instruction.intrinsic == IntrinsicKind::SourceFunctionName;
  const bool number = instruction.intrinsic == IntrinsicKind::SourceLine ||
                      instruction.intrinsic == IntrinsicKind::SourceColumn;
  if (!text && !number)
    return std::nullopt;
  if (instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.operation != MirOperation::None ||
      instruction.info.type !=
          (text ? SemanticType::StringView : SemanticType::UInt32) ||
      instruction.info.category != ValueCategory::Value ||
      !instruction.sourceLocation || instruction.receiver ||
      !instruction.operands.empty() || !instruction.parameterTypes.empty() ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget || instruction.nativeCalleeType ||
      instruction.nativeCallbackAdapter || instruction.threadTaskAdapter ||
      !instruction.scopedThreadTasks.empty() ||
      instruction.callableInvocation || instruction.callableBoundary ||
      !instruction.callableArguments.empty() ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.rawMemoryAccess ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() || !instruction.lifecycle.empty())
    throw std::logic_error(
        "LLVM source metadata requires an exact sealed literal operation");
  const auto &location = *instruction.sourceLocation;
  switch (instruction.intrinsic) {
  case IntrinsicKind::SourceFileName:
    return Literal{location.file};
  case IntrinsicKind::SourceFunctionName:
    return Literal{location.function};
  case IntrinsicKind::SourceLine:
    return Literal{std::uint64_t(location.line)};
  case IntrinsicKind::SourceColumn:
    return Literal{std::uint64_t(location.column)};
  default:
    throw std::logic_error("unclassified LLVM source metadata literal");
  }
}

} // namespace lang::native_llvm
