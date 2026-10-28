#pragma once

#include "gti/llvm_admission.h"
#include "session.h"
#if defined(GTI_LLVM_C04_MIR_EVALUATION)
#include <llvm/IR/IRBuilder.h>
#endif

namespace lang::native_llvm {

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission);

#if defined(GTI_LLVM_C04_MIR_EVALUATION)
// Isolated evaluation binaries only; not built into the installed backend.
enum class C04EvaluationProfile {
  ScalarCleanup,
  NativeBoundary,
  DynamicCalls,
  ScopedWorkers
};
class Lifetimes;
std::vector<HirFunctionInstanceId>
c04ScopedTargets(const Session &session, MirBodyAddress address,
                 const MirInstruction &instruction);
llvm::Value *emitC04Scope(Session &session, llvm::IRBuilder<> &ir,
                          MirBodyAddress address,
                          const MirInstruction &instruction,
                          llvm::ArrayRef<llvm::Value *> inputs,
                          llvm::Value *failure, Lifetimes &lifetimes);
std::vector<MirBodyAddress> validateC04Evaluation(
    const Session &session, const LlvmAdmissionPlan &admission,
    std::span<const HirFunctionInstanceId> functions,
    C04EvaluationProfile profile = C04EvaluationProfile::ScalarCleanup);
void lowerC04Evaluation(
    Session &session, const LlvmAdmissionPlan &admission,
    std::span<const HirFunctionInstanceId> functions, bool fixedCallBoundaries,
    C04EvaluationProfile profile = C04EvaluationProfile::ScalarCleanup);
#endif

} // namespace lang::native_llvm
