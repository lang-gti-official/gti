#pragma once

#include "gti/llvm_admission.h"
#include "session.h"

namespace lang::native_llvm {

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission);

#if defined(GTI_LLVM_C04_MIR_EVALUATION)
// Isolated evaluation binaries only; not built into the installed backend.
void validateC04Evaluation(const Session &session,
                           const LlvmAdmissionPlan &admission,
                           std::span<const HirFunctionInstanceId> functions);
void lowerC04Evaluation(Session &session, const LlvmAdmissionPlan &admission,
                        std::span<const HirFunctionInstanceId> functions);
#endif

} // namespace lang::native_llvm
