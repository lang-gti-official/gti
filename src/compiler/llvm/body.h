#pragma once

#include "gti/llvm_admission.h"
#include "session.h"

namespace lang::native_llvm {

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission);

} // namespace lang::native_llvm
