#pragma once

#include "gti/llvm_admission.h"
#include "session.h"

namespace lang::native_llvm {

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission);

#if defined(GTI_LLVM_C04_MIR_EVALUATION)
// Isolated evaluation binaries only; not built into the installed backend.
enum class C04EvaluationProfile { ScalarCleanup, NativeBoundary };
void validateC04Evaluation(const Session &session,
                           const LlvmAdmissionPlan &admission,
                           std::span<const HirFunctionInstanceId> functions,
                           C04EvaluationProfile profile =
                               C04EvaluationProfile::ScalarCleanup);
void lowerC04Evaluation(Session &session, const LlvmAdmissionPlan &admission,
                        std::span<const HirFunctionInstanceId> functions,
                        bool fixedCallBoundaries,
                        C04EvaluationProfile profile =
                            C04EvaluationProfile::ScalarCleanup);
#endif

} // namespace lang::native_llvm
