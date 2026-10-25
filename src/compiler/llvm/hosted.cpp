#include "hosted.h"

#include <stdexcept>

namespace lang::native_llvm {

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
