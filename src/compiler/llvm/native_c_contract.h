#pragma once

#include "gti/lowered_program.h"

namespace lang::native_llvm {

enum class NativeCValueRole { Argument, Result, Storage };

// Representation eligibility for an already verified program, not a source
// type checker or a register classifier. The physical planner consumes the
// same bounded family and separately verifies LLVM's target/layout contract.
[[nodiscard]] bool nativeCValueContract(const MirProgram &program,
                                        const TargetInfo &target,
                                        const SemanticType &type,
                                        NativeCValueRole role);
[[nodiscard]] bool
nativeCDeclarationContract(const MirProgram &program, const TargetInfo &target,
                           const MirFunctionInstance &function);
[[nodiscard]] bool nativeCDirectCallContract(const MirProgram &program,
                                             const TargetInfo &target,
                                             const MirInstruction &instruction);
[[nodiscard]] bool
nativeCIndirectCallContract(const MirProgram &program, const TargetInfo &target,
                            const MirBody &body,
                            const MirInstruction &instruction);

} // namespace lang::native_llvm
